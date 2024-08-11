#include "wcap.h"

#include "wcap_encoder.h"

#include <evr.h>
#include <cguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include "wcap_resize_shader.h"
#include "wcap_convert_shader.h"

#define MFT64(high, low) (((UINT64)high << 32) | (low))

static HRESULT STDMETHODCALLTYPE Encoder__QueryInterface(IMFAsyncCallback* this, REFIID riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IMFAsyncCallback, riid))
	{
		*Object = this;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Encoder__AddRef(IMFAsyncCallback* this)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Encoder__Release(IMFAsyncCallback* this)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Encoder__GetParameters(IMFAsyncCallback* this, DWORD* Flags, DWORD* Queue)
{
	*Flags = 0;
	*Queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__VideoInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	IUnknown* Object;
	IMFSample* Sample;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new frame submission

	Encoder* Encoder = CONTAINING_RECORD(this, struct Encoder, VideoSampleCallback);
	InterlockedIncrement(&Encoder->VideoCount);

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__AudioInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	IUnknown* Object;
	IMFSample* Sample;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new sample submission

	Encoder* Encoder = CONTAINING_RECORD(this, struct Encoder, AudioSampleCallback);
	InterlockedIncrement(&Encoder->AudioCount);
	WakeByAddressSingle(&Encoder->AudioCount);

	return S_OK;
}

static IMFAsyncCallbackVtbl Encoder__VideoSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__VideoInvoke,
};

static IMFAsyncCallbackVtbl Encoder__AudioSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__AudioInvoke,
};

static void Encoder__OutputAudioSamples(Encoder* Encoder)
{
	for (;;)
	{
		// we don't want to drop any audio frames, so wait for available sample/buffer
		LONG Count = Encoder->AudioCount;
		while (Count == 0)
		{
			LONG Zero = 0;
			WaitOnAddress(&Encoder->AudioCount, &Zero, sizeof(LONG), INFINITE);
			Count = Encoder->AudioCount;
		}

		DWORD Index = Encoder->AudioIndex;

		IMFSample* Sample = Encoder->AudioSample[Index];

		DWORD Status;
		MFT_OUTPUT_DATA_BUFFER Output = { .dwStreamID = 0, .pSample = Sample };
		HRESULT hr = IMFTransform_ProcessOutput(Encoder->Resampler, 0, 1, &Output, &Status);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			// no output is available
			break;
		}
		Assert(SUCCEEDED(hr));

		Encoder->AudioIndex = (Index + 1) % ENCODER_AUDIO_BUFFER_COUNT;
		InterlockedDecrement(&Encoder->AudioCount);

		IMFTrackedSample* Tracked;
		HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
		HR(IMFTrackedSample_SetAllocator(Tracked, &Encoder->AudioSampleCallback, (IUnknown*)Tracked));

		HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->AudioStreamIndex, Sample));

		IMFSample_Release(Sample);
		IMFTrackedSample_Release(Tracked);
	}
}

void Encoder_Init(Encoder* Encoder)
{
	HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	Encoder->VideoSampleCallback.lpVtbl = &Encoder__VideoSampleCallbackVtbl;
	Encoder->AudioSampleCallback.lpVtbl = &Encoder__AudioSampleCallbackVtbl;
}

BOOL Encoder_Start(Encoder* Encoder, ID3D11Device* Device, LPWSTR FileName, const EncoderConfig* Config)
{
	UINT Token;
	IMFDXGIDeviceManager* Manager;
	HR(MFCreateDXGIDeviceManager(&Token, &Manager));
	HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Device, Token));

	ID3D11DeviceContext* Context;
	ID3D11Device_GetImmediateContext(Device, &Context);

	ID3D11ComputeShader* ResizeShader;
	ID3D11ComputeShader* ConvertShader;
	ID3D11Device_CreateComputeShader(Device, ResizeShaderBytes, sizeof(ResizeShaderBytes), NULL, &ResizeShader);
	ID3D11Device_CreateComputeShader(Device, ConvertShaderBytes, sizeof(ConvertShaderBytes), NULL, &ConvertShader);

	DWORD InputWidth = Config->Width;
	DWORD InputHeight = Config->Height;
	DWORD OutputWidth = Config->Config->VideoMaxWidth;
	DWORD OutputHeight = Config->Config->VideoMaxHeight;

	if (OutputWidth != 0 && OutputHeight == 0)
	{
		// limit max width
		if (OutputWidth < InputWidth)
		{
			OutputHeight = InputHeight * OutputWidth / InputWidth;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth == 0 && OutputHeight != 0)
	{
		// limit max height
		if (OutputHeight < InputHeight)
		{
			OutputWidth = InputWidth * OutputHeight / InputHeight;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth != 0 && OutputHeight != 0)
	{
		// limit max width and max height
		if (OutputWidth * InputHeight < OutputHeight * InputWidth)
		{
			if (OutputWidth < InputWidth)
			{
				OutputHeight = InputHeight * OutputWidth / InputWidth;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
		else
		{
			if (OutputHeight < InputHeight)
			{
				OutputWidth = InputWidth * OutputHeight / InputHeight;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
	}
	else
	{
		// use original size
		OutputWidth = InputWidth;
		OutputHeight = InputHeight;
	}

	// must be multiple of 2, round upwards
	InputWidth = (InputWidth + 1) & ~1;
	InputHeight = (InputHeight + 1) & ~1;
	OutputWidth = (OutputWidth + 1) & ~1;
	OutputHeight = (OutputHeight + 1) & ~1;

	BOOL Result = FALSE;
	IMFSinkWriter* Writer = NULL;
	IMFTransform* Resampler = NULL;
	HRESULT hr;

	Encoder->VideoStreamIndex = -1;
	Encoder->AudioStreamIndex = -1;

	const GUID* Container;
	const GUID* Codec;
	UINT32 Profile;
	const GUID* MediaFormatYUV;
	DXGI_FORMAT FormatYUV;
	DXGI_FORMAT FormatY;
	DXGI_FORMAT FormatUV;
	if (Config->Config->VideoCodec == CONFIG_VIDEO_H264)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		FormatYUV = DXGI_FORMAT_NV12;
		FormatY = DXGI_FORMAT_R8_UINT;
		FormatUV = DXGI_FORMAT_R8G8_UINT;

		Container = Config->Config->FragmentedOutput ? &MFTranscodeContainerType_FMPEG4 : &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_H264;
		Profile = ((UINT32[]) { eAVEncH264VProfile_Base, eAVEncH264VProfile_Main, eAVEncH264VProfile_High })[Config->Config->VideoProfile];

	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		FormatYUV = DXGI_FORMAT_NV12;
		FormatY = DXGI_FORMAT_R8_UINT;
		FormatUV = DXGI_FORMAT_R8G8_UINT;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_8;

	}
	else // Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN_10
	{
		MediaFormatYUV = &MFVideoFormat_P010;
		FormatYUV = DXGI_FORMAT_P010;
		FormatY = DXGI_FORMAT_R16_UINT;
		FormatUV = DXGI_FORMAT_R16G16_UINT;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_10;
	}

	// output file
	{
		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 4));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, Config->Config->HardwareEncoder));
		HR(IMFAttributes_SetUnknown(Attributes, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown*)Manager));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
		HR(IMFAttributes_SetGUID(Attributes, &MF_TRANSCODE_CONTAINERTYPE, Container));

		hr = MFCreateSinkWriterFromURL(FileName, NULL, Attributes, &Writer);
		IMFAttributes_Release(Attributes);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot create output mp4 file!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video output type
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));

		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_MPEG2_PROFILE, Profile));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_AVG_BITRATE, Config->Config->VideoBitrate * 1000));

		hr = IMFSinkWriter_AddStream(Writer, Type, (DWORD*)&Encoder->VideoStreamIndex);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure video encoder!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video input type, NV12 or P010 format
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, MediaFormatYUV));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));

		hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->VideoStreamIndex, Type, NULL);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure video encoder input!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video encoder parameters
	{
		ICodecAPI* Codec;
		HR(IMFSinkWriter_GetServiceForStream(Writer, 0, &GUID_NULL, &IID_ICodecAPI, (LPVOID*)&Codec));

		// VBR rate control
		VARIANT RateControl = { .vt = VT_UI4, .ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonRateControlMode, &RateControl);

		// VBR bitrate to use, some MFT encoders override MF_MT_AVG_BITRATE setting with this one
		VARIANT Bitrate = { .vt = VT_UI4, .ulVal = Config->Config->VideoBitrate * 1000 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonMeanBitRate, &Bitrate);

		// set GOP size to 4 seconds
		VARIANT GopSize = { .vt = VT_UI4, .ulVal = MUL_DIV_ROUND_UP(4, Config->FramerateNum, Config->FramerateDen) };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVGOPSize, &GopSize);

		// disable low latency, for higher quality & better performance
		VARIANT LowLatency = { .vt = VT_BOOL, .boolVal = VARIANT_FALSE };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVLowLatencyMode, &LowLatency);

		// enable 2 B-frames, for better compression
		VARIANT Bframes = { .vt = VT_UI4, .ulVal = 2 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVDefaultBPictureCount, &Bframes);

		ICodecAPI_Release(Codec);
	}

	if (Config->AudioFormat)
	{
		HR(CoCreateInstance(&CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (LPVOID*)&Resampler));

		// audio resampler input
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, Config->AudioFormat, sizeof(*Config->AudioFormat) + Config->AudioFormat->cbSize));
			HR(IMFTransform_SetInputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		// audio resampler output
		{
			WAVEFORMATEX Format =
			{
				.wFormatTag = WAVE_FORMAT_PCM,
				.nChannels = (WORD)Config->Config->AudioChannels,
				.nSamplesPerSec = Config->Config->AudioSamplerate,
				.wBitsPerSample = sizeof(short) * 8,
			};
			Format.nBlockAlign = Format.nChannels * Format.wBitsPerSample / 8;
			Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, &Format, sizeof(Format)));
			HR(IMFTransform_SetOutputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		HR(IMFTransform_ProcessMessage(Resampler, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

		// audio output type
		{
			const GUID* Codec = &((GUID[]){ MFAudioFormat_AAC, MFAudioFormat_FLAC })[Config->Config->AudioCodec];

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));
			if (Config->Config->AudioCodec == CONFIG_AUDIO_AAC)
			{
				HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, Config->Config->AudioBitrate * 1000 / 8));
			}

			hr = IMFSinkWriter_AddStream(Writer, Type, (DWORD*)&Encoder->AudioStreamIndex);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				MessageBoxW(NULL, L"Cannot configure audio encoder output!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}

		// audio input type
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));

			hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->AudioStreamIndex, Type, NULL);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				MessageBoxW(NULL, L"Cannot configure audio encoder input!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}
	}

	hr = IMFSinkWriter_BeginWriting(Writer);
	if (FAILED(hr))
	{
		MessageBoxW(NULL, L"Cannot start writing to mp4 file!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}

	// video texture/buffers/samples
	{
		// RGB input texture
		{
			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = InputWidth,
				.Height = InputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
			};
			ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Encoder->InputTexture);
			ID3D11Device_CreateRenderTargetView(Device, (ID3D11Resource*)Encoder->InputTexture, NULL, &Encoder->InputRenderTarget);
			ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Encoder->InputTexture, NULL, &Encoder->ResizeInputView);

			FLOAT Black[] = { 0, 0, 0, 0 };
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputRenderTarget, Black);
		}

		// RGB resized texture
		if (InputWidth == OutputWidth && InputHeight == OutputHeight)
		{
			// no resizing needed, use input texture as input to converter shader directly
			ID3D11ShaderResourceView_AddRef(Encoder->ResizeInputView);
			Encoder->ConvertInputView = Encoder->ResizeInputView;
			Encoder->ResizedTexture = NULL;
		}
		else
		{
			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = OutputWidth,
				.Height = OutputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
			};
			ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Encoder->ResizedTexture);

			D3D11_SHADER_RESOURCE_VIEW_DESC ResourceView =
			{
				.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D.MipLevels = 1,
				.Texture2D.MostDetailedMip = 0,
			};
			ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Encoder->ResizedTexture, &ResourceView, &Encoder->ConvertInputView);

			// because D3D 11.0 does not support B8G8R8A8_UNORM for UAV, create uint UAV used on BGRA texture
			D3D11_UNORDERED_ACCESS_VIEW_DESC AccessViewDesc =
			{
				.Format = DXGI_FORMAT_R32_UINT,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D.MipSlice = 0,
			};
			ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Encoder->ResizedTexture, &AccessViewDesc, &Encoder->ResizeOutputView);
		}

		// YUV converted texture
		{
			UINT32 Size;
			HR(MFCalculateImageSize(MediaFormatYUV, OutputWidth, OutputHeight, &Size));

			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = OutputWidth,
				.Height = OutputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = FormatYUV,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC ViewY =
			{
				.Format = FormatY,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC ViewUV =
			{
				.Format = FormatUV,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			};

			// create samples each referencing individual element of texture array
			for (int i = 0; i < ENCODER_VIDEO_BUFFER_COUNT; i++)
			{
				IMFSample* VideoSample;
				IMFMediaBuffer* Buffer;

				ID3D11Texture2D* Texture;
				ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Texture);
				ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Texture, &ViewY,  &Encoder->ConvertOutputViewY[i]);
				ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Texture, &ViewUV, &Encoder->ConvertOutputViewUV[i]);
				HR(MFCreateVideoSampleFromSurface(NULL, &VideoSample));
				HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)Texture, 0, FALSE, &Buffer));
				HR(IMFMediaBuffer_SetCurrentLength(Buffer, Size));
				HR(IMFSample_AddBuffer(VideoSample, Buffer));
				IMFMediaBuffer_Release(Buffer);

				Encoder->ConvertTexture[i] = Texture;
				Encoder->VideoSample[i] = VideoSample;
			}
		}

		Encoder->InputWidth = Config->Width;
		Encoder->InputHeight = Config->Height;
		Encoder->OutputWidth = OutputWidth;
		Encoder->OutputHeight = OutputHeight;
		Encoder->FramerateNum = Config->FramerateNum;
		Encoder->FramerateDen = Config->FramerateDen;
		Encoder->VideoDiscontinuity = FALSE;
		Encoder->VideoLastTime = 0x8000000000000000ULL; // some large time in future
		Encoder->VideoIndex = 0;
		Encoder->VideoCount = ENCODER_VIDEO_BUFFER_COUNT;
	}

	if (Encoder->AudioStreamIndex >= 0)
	{
		// resampler input buffer/sample
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;

			HR(MFCreateSample(&Sample));
			HR(MFCreateMemoryBuffer(Config->AudioFormat->nAvgBytesPerSec, &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));

			Encoder->AudioInputSample = Sample;
			Encoder->AudioInputBuffer = Buffer;
		}

		// resampler output & audio encoding input buffer/samples
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;
			IMFTrackedSample* Tracked;

			HR(MFCreateTrackedSample(&Tracked));
			HR(IMFTrackedSample_QueryInterface(Tracked, &IID_IMFSample, (LPVOID*)&Sample));
			HR(MFCreateMemoryBuffer(Config->Config->AudioSamplerate * Config->Config->AudioChannels * sizeof(short), &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));
			IMFMediaBuffer_Release(Buffer);
			IMFTrackedSample_Release(Tracked);

			Encoder->AudioSample[i] = Sample;
		}

		Encoder->AudioFrameSize = Config->AudioFormat->nBlockAlign;
		Encoder->AudioSampleRate = Config->AudioFormat->nSamplesPerSec;
		Encoder->Resampler = Resampler;
		Encoder->AudioIndex = 0;
		Encoder->AudioCount = ENCODER_AUDIO_BUFFER_COUNT;
	}

	// constant buffer for RGB to YUV conversion
	{
		float RangeY, OffsetY;
		float RangeUV, OffsetUV;

		if (FormatYUV == DXGI_FORMAT_NV12)
		{
			// Y=[16..235], UV=[16..240]
			RangeY = 219.f;
			OffsetY = 16.5f;
			RangeUV = 224.f;
			OffsetUV = 128.5f;
		}
		else // FormatYUV == DXGI_FORMAT_P010
		{
			// Y=[64..940], UV=[64..960]
			// mutiplied by 64, because 10-bit values are positioned at top of 16-bit used for texture storage format
			RangeY = 876.f * 64.f;
			OffsetY = 64.5f * 64.f;
			RangeUV = 896.f * 64.f;
			OffsetUV = 512.5f * 64.f;
		}

		// BT.709 - https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion
		float ConvertMtx[3][4] =
		{
			{  0.2126f * RangeY,   0.7152f * RangeY,   0.0722f * RangeY,  OffsetY  },
			{ -0.1146f * RangeUV, -0.3854f * RangeUV,  0.5f    * RangeUV, OffsetUV },
			{  0.5f    * RangeUV, -0.4542f * RangeUV, -0.0458f * RangeUV, OffsetUV },
		};

		D3D11_BUFFER_DESC Desc =
		{
			.ByteWidth = sizeof(ConvertMtx),
			.Usage = D3D11_USAGE_IMMUTABLE,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};

		D3D11_SUBRESOURCE_DATA Data =
		{
			.pSysMem = ConvertMtx,
		};

		ID3D11Device_CreateBuffer(Device, &Desc, &Data, &Encoder->ConvertBuffer);
	}

	ID3D11Device_AddRef(Device);
	ID3D11DeviceContext_AddRef(Context);
	ID3D11ComputeShader_AddRef(ResizeShader);
	ID3D11ComputeShader_AddRef(ConvertShader);
	Encoder->Context = Context;
	Encoder->Device = Device;
	Encoder->ResizeShader = ResizeShader;
	Encoder->ConvertShader = ConvertShader;

	Encoder->StartTime = 0;
	Encoder->Writer = Writer;
	Writer = NULL;
	Resampler = NULL;
	Result = TRUE;

bail:
	if (Resampler)
	{
		IMFTransform_Release(Resampler);
	}
	if (Writer)
	{
		IMFSinkWriter_Release(Writer);
		DeleteFileW(FileName);
	}
	ID3D11ComputeShader_Release(ConvertShader);
	ID3D11ComputeShader_Release(ResizeShader);
	ID3D11DeviceContext_Release(Context);
	IMFDXGIDeviceManager_Release(Manager);

	return Result;
}

void Encoder_Stop(Encoder* Encoder)
{
	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFTransform_ProcessMessage(Encoder->Resampler, MFT_MESSAGE_COMMAND_DRAIN, 0));
		Encoder__OutputAudioSamples(Encoder);
		IMFTransform_Release(Encoder->Resampler);
	}

	IMFSinkWriter_Finalize(Encoder->Writer);
	IMFSinkWriter_Release(Encoder->Writer);

	if (Encoder->AudioStreamIndex >= 0)
	{
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample_Release(Encoder->AudioSample[i]);
		}
		IMFSample_Release(Encoder->AudioInputSample);
		IMFMediaBuffer_Release(Encoder->AudioInputBuffer);
	}

	for (int i = 0; i < ENCODER_VIDEO_BUFFER_COUNT; i++)
	{
		ID3D11UnorderedAccessView_Release(Encoder->ConvertOutputViewY[i]);
		ID3D11UnorderedAccessView_Release(Encoder->ConvertOutputViewUV[i]);
		ID3D11Texture2D_Release(Encoder->ConvertTexture[i]);
		IMFSample_Release(Encoder->VideoSample[i]);
	}

    ID3D11ShaderResourceView_Release(Encoder->ConvertInputView);
	if (Encoder->ResizedTexture)
	{
		ID3D11UnorderedAccessView_Release(Encoder->ResizeOutputView);
		ID3D11Texture2D_Release(Encoder->ResizedTexture);
		Encoder->ResizedTexture = NULL;
	}

	ID3D11RenderTargetView_Release(Encoder->InputRenderTarget);
	ID3D11ShaderResourceView_Release(Encoder->ResizeInputView);
	ID3D11Texture2D_Release(Encoder->InputTexture);

	ID3D11Buffer_Release(Encoder->ConvertBuffer);
	ID3D11ComputeShader_Release(Encoder->ResizeShader);
	ID3D11ComputeShader_Release(Encoder->ConvertShader);
	ID3D11DeviceContext_Release(Encoder->Context);
	ID3D11Device_Release(Encoder->Device);
}

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod)
{
	Encoder->VideoLastTime = Time;

	if (Encoder->VideoCount == 0)
	{
		// dropped frame
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
		return FALSE;
	}
	DWORD Index = Encoder->VideoIndex;
	Encoder->VideoIndex = (Index + 1) % ENCODER_VIDEO_BUFFER_COUNT;
	InterlockedDecrement(&Encoder->VideoCount);

	IMFSample* Sample = Encoder->VideoSample[Index];

	ID3D11DeviceContext* Context = Encoder->Context;

	// copy to input texture
	{
		D3D11_BOX Box =
		{
			.left = Rect.left,
			.top = Rect.top,
			.right = Rect.right,
			.bottom = Rect.bottom,
			.front = 0,
			.back = 1,
		};

		DWORD Width = Box.right - Box.left;
		DWORD Height = Box.bottom - Box.top;
		if (Width < Encoder->InputWidth || Height < Encoder->InputHeight)
		{
			FLOAT Black[] = { 0, 0, 0, 0 };
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputRenderTarget, Black);

			Box.right = Box.left + min(Encoder->InputWidth, Box.right);
			Box.bottom = Box.top + min(Encoder->InputHeight, Box.bottom);
		}
		ID3D11DeviceContext_CopySubresourceRegion(Context, (ID3D11Resource*)Encoder->InputTexture, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	}

	// resize if needed
	if (Encoder->ResizedTexture != NULL)
	{
		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Encoder->ResizeInputView);
		// output
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Encoder->ResizeOutputView, NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ResizeShader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, (Encoder->OutputWidth + 15) / 16, (Encoder->OutputHeight + 7) / 8, 1);
	}

	// convert to YUV
	{
		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Encoder->ConvertBuffer);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Encoder->ConvertInputView);
		// output
		ID3D11UnorderedAccessView* Views[] = { Encoder->ConvertOutputViewY[Index], Encoder->ConvertOutputViewUV[Index] };
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, _countof(Views), Views, NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ConvertShader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, (Encoder->OutputWidth / 2 + 15) / 16, (Encoder->OutputHeight / 2 + 7) / 8, 1);
	}

	// setup input time & duration
	if (Encoder->StartTime == 0)
	{
		Encoder->StartTime = Time;
	}
	HR(IMFSample_SetSampleDuration(Sample, MFllMulDiv(Encoder->FramerateDen, MF_UNITS_PER_SECOND, Encoder->FramerateNum, 0)));
	HR(IMFSample_SetSampleTime(Sample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	if (Encoder->VideoDiscontinuity)
	{
		HR(IMFSample_SetUINT32(Sample, &MFSampleExtension_Discontinuity, TRUE));
		Encoder->VideoDiscontinuity = FALSE;
	}
	else
	{
		// don't care about success or no, we just don't want this attribute set at all
		IMFSample_DeleteItem(Sample, &MFSampleExtension_Discontinuity);
	}

	IMFTrackedSample* Tracked;
	HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
	IMFTrackedSample_SetAllocator(Tracked, &Encoder->VideoSampleCallback, NULL);
	IMFTrackedSample_Release(Tracked);

	// submit to encoder which will happen in background
	HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->VideoStreamIndex, Sample));

	IMFSample_Release(Sample);

	return TRUE;
}

void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD VideoCount, UINT64 Time, UINT64 TimePeriod)
{
	IMFSample* AudioSample = Encoder->AudioInputSample;
	IMFMediaBuffer* Buffer = Encoder->AudioInputBuffer;

	BYTE* BufferData;
	DWORD MaxLength;
	HR(IMFMediaBuffer_Lock(Buffer, &BufferData, &MaxLength, NULL));

	DWORD BufferSize = VideoCount * Encoder->AudioFrameSize;
	Assert(BufferSize <= MaxLength);

	if (Samples)
	{
		CopyMemory(BufferData, Samples, BufferSize);
	}
	else
	{
		ZeroMemory(BufferData, BufferSize);
	}

	HR(IMFMediaBuffer_Unlock(Buffer));
	HR(IMFMediaBuffer_SetCurrentLength(Buffer, BufferSize));

	// setup input time & duration
	Assert(Encoder->StartTime != 0);
	HR(IMFSample_SetSampleDuration(AudioSample, MFllMulDiv(VideoCount, MF_UNITS_PER_SECOND, Encoder->AudioSampleRate, 0)));
	HR(IMFSample_SetSampleTime(AudioSample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	HR(IMFTransform_ProcessInput(Encoder->Resampler, 0, AudioSample, 0));
	Encoder__OutputAudioSamples(Encoder);
}

void Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod)
{
	// if there was no frame during last second, add discontinuity
	if (Time - Encoder->VideoLastTime >= TimePeriod)
	{
		Encoder->VideoLastTime = Time;
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
	}
}

void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize)
{
	MF_SINK_WRITER_STATISTICS Stats = { .cb = sizeof(Stats) };
	HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->VideoStreamIndex, &Stats));

	*Bitrate = (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
	*LengthMsec = (DWORD)(Stats.llLastTimestampProcessed / 10000);
	*FileSize = Stats.qwByteCountProcessed;

	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->AudioStreamIndex, &Stats));
		*Bitrate += (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
		*FileSize += Stats.qwByteCountProcessed;
	}
}
