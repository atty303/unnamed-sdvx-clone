#include "stdafx.h"
#include "AudioOutput.hpp"
#include "Shared/Thread.hpp"

// This audio driver has the least latency on windows
#ifdef _WIN32
#include "Audioclient.h"
#include "Mmdeviceapi.h"
#include "comdef.h"
#include "Functiondiscoverykeys_devpkey.h"

#define REFTIME_NS (100)
#define REFTIMES_PER_MICROSEC (1000/REFTIME_NS)
#define REFTIMES_PER_MILLISEC (REFTIMES_PER_MICROSEC * 1000)
#define REFTIMES_PER_SEC  (REFTIMES_PER_MILLISEC * 1000)
#define SAFE_RELEASE(punk)  \
              if ((punk) != NULL)  \
                { (punk)->Release(); (punk) = nullptr; }

static const uint32_t freq = 44100;
static const uint32_t channels = 2;
static const uint32_t numBuffers = 2;
static const uint32_t bufferLength = 10;
static const REFERENCE_TIME bufferDuration = (REFERENCE_TIME)(bufferLength * REFTIMES_PER_MILLISEC);

// Object that handles the addition/removal of audio devices
class NotificationClient : public IMMNotificationClient
{
public:
	class AudioOutput_Impl* output;

	virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(_In_ LPCWSTR pwstrDeviceId, _In_ DWORD dwNewState);
	virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded(_In_ LPCWSTR pwstrDeviceId);
	virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved(_In_ LPCWSTR pwstrDeviceId);
	virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(_In_ EDataFlow flow, _In_ ERole role, _In_ LPCWSTR pwstrDefaultDeviceId);
	virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(_In_ LPCWSTR pwstrDeviceId, _In_ const PROPERTYKEY key);

	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject)
	{
		return E_NOINTERFACE;
	}
	virtual ULONG STDMETHODCALLTYPE AddRef(void)
	{
		return 0;
	}
	virtual ULONG STDMETHODCALLTYPE Release(void)
	{
		return 0;
	}
};

class AudioOutput_Impl
{
public:
	struct IAudioClient* m_audioClient = nullptr;
	struct IAudioRenderClient* m_audioRenderClient = nullptr;
	struct IMMDevice* m_device = nullptr;
	tWAVEFORMATEX m_format;
	IMMDeviceEnumerator* m_deviceEnumerator = nullptr;
	// The output wave buffer
	uint32_t m_numBufferFrames;

	// Object that receives device change notifications
	NotificationClient m_notificationClient;

	double m_bufferLength;

	// Dummy audio output
	static const uint32 m_dummyChannelCount = 2;
	static const uint32 m_dummyBufferLength = (uint32)((double)freq * 0.2);
	float m_dummyBuffer[m_dummyBufferLength * m_dummyChannelCount];
	double m_dummyTimerPos;
	Timer m_dummyTimer;

	// Set if the device should change soon
	IMMDevice* m_pendingDevice = nullptr;
	bool m_pendingDeviceChange = false;

	bool m_runAudioThread = false;
	Thread m_audioThread;
	IMixer* m_mixer = nullptr;
	bool m_exclusive = false;

public:
	AudioOutput_Impl()
	{
		m_notificationClient.output = this;

	}
	~AudioOutput_Impl()
	{
		// Stop thread
		Stop();

		CloseDevice();

		SAFE_RELEASE(m_pendingDevice);
		if(m_deviceEnumerator)
		{
			m_deviceEnumerator->UnregisterEndpointNotificationCallback(&m_notificationClient);
			SAFE_RELEASE(m_deviceEnumerator);
		}
	}

	void Start()
	{
		if(m_runAudioThread)
			return;

		m_runAudioThread = true;
		m_audioThread = Thread(&AudioOutput_Impl::AudioThread, this);
	}
	void Stop()
	{
		if(!m_runAudioThread)
			return;

		// Join audio thread
		m_runAudioThread = false;
		if(m_audioThread.joinable())
			m_audioThread.join();
	}

	bool Init(bool exclusive)
	{
		m_exclusive = exclusive;

		// Initialize the WASAPI device enumerator
		HRESULT res;
		const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
		const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
		CoInitialize(nullptr);
		if(!m_deviceEnumerator)
		{
			res = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&m_deviceEnumerator);
			if(res != S_OK)
				throw _com_error(res);

			// Register change handler
			m_deviceEnumerator->RegisterEndpointNotificationCallback(&m_notificationClient);
		}

		// Select default device
		IMMDevice* defaultDevice = nullptr;
		m_deviceEnumerator->GetDefaultAudioEndpoint(EDataFlow::eRender, ERole::eMultimedia, &defaultDevice);		
		return OpenDevice(defaultDevice);
	}	
	void CloseDevice()
	{
		if(m_audioClient)
			m_audioClient->Stop();
		SAFE_RELEASE(m_device);
		SAFE_RELEASE(m_audioClient);
		SAFE_RELEASE(m_audioRenderClient);
	}
	IMMDevice* FindDevice(LPCWSTR devId)
	{
		assert(m_deviceEnumerator);
		IMMDevice* newDevice = nullptr;
		if(m_deviceEnumerator->GetDevice(devId, &newDevice) != S_OK)
		{
			SAFE_RELEASE(newDevice);
			return nullptr;
		}
		return newDevice;
	}
	bool OpenDevice(LPCWSTR devId)
	{
		return OpenDevice(FindDevice(devId));
	}
	bool OpenDevice(IMMDevice* device)
	{
		// Close old device first
		CloseDevice();

		// Open dummy device when no device specified
		if(!device)
			return OpenNullDevice();

		HRESULT res = 0;

		// Obtain audio client
		m_device = device;
		res = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
		if(res != S_OK)
			throw _com_error(res);

		WAVEFORMATEX* mixFormat = nullptr;
		res = m_audioClient->GetMixFormat(&mixFormat);
		if (m_exclusive)
		{
			// Aquire format and initialize device for exclusive mode
			REFERENCE_TIME defaultDevicePeriod, minDevicePeriod;
			m_audioClient->GetDevicePeriod(&defaultDevicePeriod, &minDevicePeriod);
			res = m_audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mixFormat, NULL);
			if (res == AUDCLNT_E_UNSUPPORTED_FORMAT)
			{
				Log("Default format not supported in exclusive mode, attempting other formats", Logger::Error);

				int numFormats = 2;
				WORD formats[2] = { WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT };

				int numRates = 5;
				long sampleRates[5] = {192000L, 96000L, 88200L, 48000L, 44100L};

				int numDepths = 3;
				int bitDepths[3] = {32, 24, 16 };
				
				Vector<WAVEFORMATEX> allFormats;
				for (size_t f = 0; f < numFormats; f++)
				{
					for (size_t d = 0; d < numDepths; d++)
					{
						for (size_t r = 0; r < numRates; r++)
						{
							long avgBytesPerSec = (bitDepths[d] / 8) * sampleRates[r] * 2;
							WAVEFORMATEX newformat;
							newformat.wFormatTag = formats[f];
							newformat.nChannels = 2;
							newformat.nSamplesPerSec = sampleRates[r];
							newformat.nAvgBytesPerSec = avgBytesPerSec;
							newformat.nBlockAlign = 4;
							newformat.wBitsPerSample = bitDepths[d];
							newformat.cbSize = 0;
							allFormats.Add(newformat);
						}
					}
				}

				int attemptingFormat = 0;
				while (res != S_OK)
				{
					*mixFormat = allFormats[attemptingFormat];

					Logf("Attempting exclusive mode format:\nSample Rate: %dhz,\nBit Depth: %dbit,\nFormat: %s\n-----", Logger::Info,
						mixFormat->nSamplesPerSec,
						mixFormat->wBitsPerSample,
						mixFormat->wFormatTag == WAVE_FORMAT_PCM ? "PCM" : "IEEE FLOAT"
						);

					res = m_audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mixFormat, NULL);
					attemptingFormat++;
					if (attemptingFormat >= allFormats.size())
					{
						break;
					}
				}
				if (res == S_OK)
					Log("Format successful", Logger::Info);
				else
					Log("No accepted format found", Logger::Error);

			}
			// Init client
			res = m_audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
				bufferDuration, defaultDevicePeriod, mixFormat, nullptr);
		}
		else
		{
			// Init client
			res = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
				bufferDuration, 0, mixFormat, nullptr);
		}
		// Store selected format
		m_format = *mixFormat;
		CoTaskMemFree(mixFormat);

		// Check if initialization was succesfull
		if(res != S_OK)
		{
			Log("Failed to initialize audio client with the selected settings", Logger::Error);
			SAFE_RELEASE(device);
			SAFE_RELEASE(m_audioClient);
			return false;
		}

		// Get the audio render client
		res = m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_audioRenderClient);
		if(res != S_OK)
		{
			Log("Failed to get audio render client service.", Logger::Error);
			SAFE_RELEASE(device);
			SAFE_RELEASE(m_audioClient);
			return false;
		}

		// Get the number of buffer frames
		m_audioClient->GetBufferSize(&m_numBufferFrames);

		m_bufferLength = (double)m_numBufferFrames / (double)m_format.nSamplesPerSec;

		res = m_audioClient->Start();
		return true;
	}
	bool OpenNullDevice()
	{
		m_format.nSamplesPerSec = freq;
		m_format.nChannels = 2;
		m_dummyTimer.Restart();
		m_dummyTimerPos = 0;
		return true;
	}
	bool NullBegin(float*& buffer, uint32_t& numSamples)
	{
		if(m_dummyTimer.Milliseconds() > 2)
		{
			m_dummyTimerPos += m_dummyTimer.SecondsAsDouble();
			m_dummyTimer.Restart();

			uint32 availableSamples = (uint32)(m_dummyTimerPos * (double)m_format.nSamplesPerSec);
			if(availableSamples > 0)
			{
				numSamples = Math::Min(m_dummyBufferLength, availableSamples);

				// Restart timer pos
				m_dummyTimerPos = 0;
				buffer = m_dummyBuffer;
				return true;
			}
		}
		return false;
	}

	bool Begin(float*& buffer, uint32_t& numSamples)
	{
		if(m_pendingDeviceChange)
		{
			OpenDevice(m_pendingDevice);
			m_pendingDeviceChange = false;
		}
		if(!m_device)
			return NullBegin(buffer, numSamples);

		// See how much buffer space is available.
		uint32_t numFramesPadding;
		m_audioClient->GetCurrentPadding(&numFramesPadding);
		numSamples = m_numBufferFrames - numFramesPadding;

		if(numSamples > 0)
		{
			// Grab all the available space in the shared buffer.
			HRESULT hr = m_audioRenderClient->GetBuffer(numSamples, (BYTE**)&buffer);
			if(hr != S_OK)
			{
				if(hr == AUDCLNT_E_DEVICE_INVALIDATED)
				{
					Logf("Audio device unplugged", Logger::Warning);
					return false;
				}
				else
				{
					assert(false);
				}
			}
			return true;
		}
		return false;
	}
	void End(uint32_t numSamples)
	{
		if(!m_device)
			return;

		if(numSamples > 0)
		{
			m_audioRenderClient->ReleaseBuffer(numSamples, 0);
		}
	}

	// Main mixer thread
	void AudioThread()
	{
		while(m_runAudioThread)
		{
			int32 sleepDuration = 1;
			float* data;
			uint32 numSamples;
			if(Begin(data, numSamples))
			{
				if(m_mixer)
					m_mixer->Mix(data, numSamples);
				End(numSamples);
			}
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
};

/* Audio change notifications */
HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceStateChanged(_In_ LPCWSTR pwstrDeviceId, _In_ DWORD dwNewState)
{
	return S_OK;
}
HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceAdded(_In_ LPCWSTR pwstrDeviceId)
{
	return S_OK;
}
HRESULT STDMETHODCALLTYPE NotificationClient::OnDeviceRemoved(_In_ LPCWSTR pwstrDeviceId)
{
	return S_OK;
}
HRESULT STDMETHODCALLTYPE NotificationClient::OnDefaultDeviceChanged(_In_ EDataFlow flow, _In_ ERole role, _In_ LPCWSTR pwstrDefaultDeviceId)
{
	if(flow == EDataFlow::eRender && role == ERole::eMultimedia)
	{
		output->m_pendingDeviceChange = true;
		output->m_pendingDevice = output->FindDevice(pwstrDefaultDeviceId);
	}
	return S_OK;
}
HRESULT STDMETHODCALLTYPE NotificationClient::OnPropertyValueChanged(_In_ LPCWSTR pwstrDeviceId, _In_ const PROPERTYKEY key)
{
	return S_OK;
}

AudioOutput::AudioOutput()
{
	m_impl = new AudioOutput_Impl();
}
AudioOutput::~AudioOutput()
{
	delete m_impl;
}
bool AudioOutput::Init(bool exclusive)
{
	return m_impl->Init(exclusive);
}
void AudioOutput::Start(IMixer* mixer)
{
	m_impl->m_mixer = mixer;
	m_impl->Start();
}
void AudioOutput::Stop()
{
	m_impl->Stop();
	m_impl->m_mixer = nullptr;
}
uint32_t AudioOutput::GetNumChannels() const
{
	return m_impl->m_format.nChannels;
}
uint32_t AudioOutput::GetSampleRate() const
{
	return m_impl->m_format.nSamplesPerSec;
}
double AudioOutput::GetBufferLength() const
{
	return m_impl->m_bufferLength;
}
bool AudioOutput::IsIntegerFormat() const
{
	///TODO: check more cases?
	return m_impl->m_format.wFormatTag == WAVE_FORMAT_PCM && m_impl->m_format.wBitsPerSample != 32;
}
#endif