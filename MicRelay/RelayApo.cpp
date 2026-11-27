#include <windows.h>
#include <sddl.h>
#include <audioenginebaseapo.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

#include "RelayBuffer.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

class RelayModule
{
public:
	static inline std::atomic<LONG> Objects = 0;
	static inline std::atomic<LONG> Locks = 0;
	static constexpr CLSID Clsid = { 0xbb0fd71a, 0x4c49, 0x4d75, { 0xb4, 0xe8, 0x49, 0x13, 0x4b, 0x79, 0x9f, 0x08 } };
	static constexpr GUID FloatFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
};

class Resampler
{
public:
	static constexpr int Lobes = 8;
	static constexpr int Resolution = 512;

	static void Build()
	{
		std::call_once(Built, [] {
			constexpr double pi = 3.14159265358979323846;
			for (int i = 0; i <= Lobes * Resolution + 1; i++) {
				const double x = static_cast<double>(i) / Resolution;
				if (x == 0.0)
					Table[i] = 1.f;
				else if (x >= Lobes)
					Table[i] = 0.f;
				else
					Table[i] = static_cast<float>((std::sin(pi * x) / (pi * x)) * (std::sin(pi * x / Lobes) / (pi * x / Lobes)));
			}
		});
	}

	static void Sample(const float* Samples, double Position, double Scale, float& Left, float& Right)
	{
		const double half = Lobes * Scale;
		const LONG64 first = static_cast<LONG64>(std::ceil(Position - half));
		const LONG64 last = static_cast<LONG64>(std::floor(Position + half));
		const double inverse = 1.0 / Scale;
		float left = 0;
		float right = 0;
		float total = 0;
		for (LONG64 k = first; k <= last; k++) {
			const float weight = Kernel((Position - static_cast<double>(k)) * inverse);
			const uint32_t index = static_cast<uint32_t>(k) & RelayBuffer::Mask;
			left += Samples[index * 2] * weight;
			right += Samples[index * 2 + 1] * weight;
			total += weight;
		}
		if (total > 0.0001f) {
			Left = left / total;
			Right = right / total;
		}
		else {
			Left = 0;
			Right = 0;
		}
	}

private:
	static float Kernel(double X)
	{
		const double x = (X < 0 ? -X : X) * Resolution;
		const int index = static_cast<int>(x);
		if (index >= Lobes * Resolution)
			return 0.f;
		const float frac = static_cast<float>(x - index);
		return Table[index] + (Table[index + 1] - Table[index]) * frac;
	}

	static inline float Table[Lobes * Resolution + 2] = {};
	static inline std::once_flag Built;
};

class RelayApo final :public IAudioProcessingObject, public IAudioProcessingObjectRT, public IAudioProcessingObjectConfiguration, public IAudioSystemEffects
{
public:
	explicit RelayApo(IUnknown* OuterUnknown) : InnerUnknown(this), Outer(OuterUnknown ? OuterUnknown : &InnerUnknown)
	{
		RelayModule::Objects++;
	}

	~RelayApo()
	{
		CloseRelay();
		RelayModule::Objects--;
	}

	IUnknown* Inner() { return &InnerUnknown; }

	STDMETHODIMP QueryInterface(REFIID Iid, void** Object) override { return Outer->QueryInterface(Iid, Object); }
	STDMETHODIMP_(ULONG) AddRef() override { return Outer->AddRef(); }
	STDMETHODIMP_(ULONG) Release() override { return Outer->Release(); }

	STDMETHODIMP Reset() override
	{
		bSynced = false;
		return S_OK;
	}

	STDMETHODIMP GetLatency(HNSTIME* Latency) override
	{
		if (!Latency)
			return E_POINTER;
		*Latency = 0;
		return S_OK;
	}

	STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** Properties) override
	{
		if (!Properties)
			return E_POINTER;
		auto* props = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
		if (!props)
			return E_OUTOFMEMORY;
		ZeroMemory(props, sizeof(APO_REG_PROPERTIES));
		props->clsid = RelayModule::Clsid;
		props->Flags = APO_FLAG_DEFAULT;
		wcscpy_s(props->szFriendlyName, L"Wave Mic Relay");
		wcscpy_s(props->szCopyrightInfo, L"Wave");
		props->u32MajorVersion = 1;
		props->u32MinorVersion = 0;
		props->u32MinInputConnections = 1;
		props->u32MaxInputConnections = 1;
		props->u32MinOutputConnections = 1;
		props->u32MaxOutputConnections = 1;
		props->u32MaxInstances = 0xFFFFFFFF;
		props->u32NumAPOInterfaces = 1;
		props->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
		*Properties = props;
		return S_OK;
	}

	STDMETHODIMP Initialize(UINT32, BYTE*) override
	{
		return S_OK;
	}

	STDMETHODIMP IsInputFormatSupported(IAudioMediaType* Opposite, IAudioMediaType* Requested, IAudioMediaType** Supported) override
	{
		return CheckFormat(Opposite, Requested, Supported);
	}

	STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* Opposite, IAudioMediaType* Requested, IAudioMediaType** Supported) override
	{
		return CheckFormat(Opposite, Requested, Supported);
	}

	STDMETHODIMP GetInputChannelCount(UINT32* Count) override
	{
		if (!Count)
			return E_POINTER;
		*Count = Channels;
		return S_OK;
	}

	STDMETHODIMP LockForProcess(UINT32 InputCount, APO_CONNECTION_DESCRIPTOR** Inputs, UINT32 OutputCount, APO_CONNECTION_DESCRIPTOR** Outputs) override
	{
		if (InputCount != 1 || OutputCount != 1 || !Inputs || !Outputs || !Inputs[0] || !Inputs[0]->pFormat)
			return E_INVALIDARG;
		UNCOMPRESSEDAUDIOFORMAT format = {};
		const HRESULT hr = Inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
		if (FAILED(hr))
			return hr;
		if (format.guidFormatType != RelayModule::FloatFormat || format.dwBytesPerSampleContainer != sizeof(float) || format.dwSamplesPerFrame == 0)
			return APOERR_FORMAT_NOT_SUPPORTED;
		Channels = format.dwSamplesPerFrame;
		FrameRate = format.fFramesPerSecond > 0 ? format.fFramesPerSecond : 48000.f;
		bSynced = false;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		QpcFrequency = frequency.QuadPart > 0 ? static_cast<double>(frequency.QuadPart) : 1.0;
		Resampler::Build();
		OpenRelay();
		bLocked = true;
		return S_OK;
	}

	STDMETHODIMP UnlockForProcess() override
	{
		bLocked = false;
		CloseRelay();
		return S_OK;
	}

	STDMETHODIMP_(void) APOProcess(UINT32 InputCount, APO_CONNECTION_PROPERTY** Inputs, UINT32 OutputCount, APO_CONNECTION_PROPERTY** Outputs) override
	{
		if (InputCount != 1 || OutputCount != 1)
			return;
		APO_CONNECTION_PROPERTY* in = Inputs[0];
		APO_CONNECTION_PROPERTY* out = Outputs[0];
		const UINT32 frames = in->u32ValidFrameCount;
		const size_t samples = static_cast<size_t>(frames) * Channels;
		float* source = reinterpret_cast<float*>(in->pBuffer);
		float* target = reinterpret_cast<float*>(out->pBuffer);

		if (in->u32BufferFlags == BUFFER_SILENT)
			memset(target, 0, samples * sizeof(float));
		else if (source != target)
			memcpy(target, source, samples * sizeof(float));

		const bool bMixed = Mix(target, frames);
		out->u32ValidFrameCount = frames;
		out->u32BufferFlags = bMixed ? BUFFER_VALID : in->u32BufferFlags;
	}

	STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 OutputFrames) override { return OutputFrames; }
	STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 InputFrames) override { return InputFrames; }

	HRESULT NonDelegatingQueryInterface(REFIID Iid, void** Object)
	{
		if (!Object)
			return E_POINTER;
		if (Iid == __uuidof(IUnknown)) {
			*Object = &InnerUnknown;
			InnerUnknown.AddRef();
			return S_OK;
		}
		if (Iid == __uuidof(IAudioProcessingObject))
			*Object = static_cast<IAudioProcessingObject*>(this);
		else if (Iid == __uuidof(IAudioProcessingObjectRT))
			*Object = static_cast<IAudioProcessingObjectRT*>(this);
		else if (Iid == __uuidof(IAudioProcessingObjectConfiguration))
			*Object = static_cast<IAudioProcessingObjectConfiguration*>(this);
		else if (Iid == __uuidof(IAudioSystemEffects))
			*Object = static_cast<IAudioSystemEffects*>(this);
		else {
			*Object = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}

	ULONG NonDelegatingAddRef() { return ++Refs; }

	ULONG NonDelegatingRelease()
	{
		const ULONG refs = --Refs;
		if (refs == 0)
			delete this;
		return refs;
	}

private:
	class InnerImpl final : public IUnknown
	{
	public:
		explicit InnerImpl(RelayApo* Owner) : Owner(Owner) {}
		STDMETHODIMP QueryInterface(REFIID Iid, void** Object) override { return Owner->NonDelegatingQueryInterface(Iid, Object); }
		STDMETHODIMP_(ULONG) AddRef() override { return Owner->NonDelegatingAddRef(); }
		STDMETHODIMP_(ULONG) Release() override { return Owner->NonDelegatingRelease(); }

	private:
		RelayApo* Owner;
	};

	static HRESULT CheckFormat(IAudioMediaType* Opposite, IAudioMediaType* Requested, IAudioMediaType** Supported)
	{
		if (!Requested || !Supported)
			return E_POINTER;
		*Supported = nullptr;
		UNCOMPRESSEDAUDIOFORMAT format = {};
		if (FAILED(Requested->GetUncompressedAudioFormat(&format)))
			return APOERR_FORMAT_NOT_SUPPORTED;
		if (format.guidFormatType != RelayModule::FloatFormat || format.dwBytesPerSampleContainer != sizeof(float))
			return APOERR_FORMAT_NOT_SUPPORTED;
		if (Opposite) {
			DWORD flags = 0;
			if (Opposite->IsEqual(Requested, &flags) != S_OK) {
				UNCOMPRESSEDAUDIOFORMAT other = {};
				if (SUCCEEDED(Opposite->GetUncompressedAudioFormat(&other)) &&
					(other.dwSamplesPerFrame != format.dwSamplesPerFrame || other.fFramesPerSecond != format.fFramesPerSecond || other.guidFormatType != format.guidFormatType)) {
					Opposite->AddRef();
					*Supported = Opposite;
					return S_FALSE;
				}
			}
		}
		Requested->AddRef();
		*Supported = Requested;
		return S_OK;
	}

	void OpenRelay()
	{
		if (View)
			return;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		SECURITY_ATTRIBUTES attributes = { sizeof(attributes), nullptr, FALSE };
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &descriptor, nullptr))
			attributes.lpSecurityDescriptor = descriptor;

		Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, attributes.lpSecurityDescriptor ? &attributes : nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(RelayBuffer::Size), RelayBuffer::MappingName);
		const bool bCreated = Mapping && GetLastError() != ERROR_ALREADY_EXISTS;
		if (descriptor)
			LocalFree(descriptor);
		if (!Mapping)
			return;

		View = static_cast<RelayBuffer::Header*>(MapViewOfFile(Mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, RelayBuffer::Size));
		if (!View) {
			CloseHandle(Mapping);
			Mapping = nullptr;
			return;
		}
		if (bCreated) {
			View->Capacity = RelayBuffer::Capacity;
			View->Version = RelayBuffer::Version;
			MemoryBarrier();
			View->Magic = RelayBuffer::Magic;
		}
	}

	void CloseRelay()
	{
		if (View)
			UnmapViewOfFile(View);
		if (Mapping)
			CloseHandle(Mapping);
		View = nullptr;
		Mapping = nullptr;
	}

	enum class ClockState { Unknown, Paused, Playing };

	ClockState ClockedPosition(const RelayBuffer::Header* Shared, LONG Rate, double& Frame)
	{
		LONG64 anchorFrame = 0;
		LONG64 anchorTime = 0;
		LONG64 clockTime = 0;
		LONG64 clockQpc = 0;
		bool bPlaying = false;
		if (!RelayBuffer::ReadAnchor(Shared, anchorFrame, anchorTime) || !RelayBuffer::ReadClock(Shared, clockTime, clockQpc, bPlaying) || anchorTime < 0)
			return ClockState::Unknown;
		if (!bPlaying)
			return ClockState::Paused;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const double elapsed = static_cast<double>(now.QuadPart - clockQpc) / QpcFrequency;
		if (elapsed < 0 || elapsed > 1.0)
			return ClockState::Unknown;
		const double mediaNow = static_cast<double>(clockTime) / 10'000'000.0 + elapsed;
		Frame = static_cast<double>(anchorFrame) + (mediaNow - static_cast<double>(anchorTime) / 10'000'000.0) * Rate;
		return ClockState::Playing;
	}

	bool Mix(float* Target, UINT32 Frames)
	{
		RelayBuffer::Header* view = View;
		if (!RelayBuffer::IsValid(view) || Frames == 0) {
			bSynced = false;
			return false;
		}
		if (GetTickCount64() - static_cast<ULONGLONG>(view->LastWriteTick) > RelayBuffer::StaleAfterMs) {
			bSynced = false;
			return false;
		}

		const LONG rate = view->SampleRate;
		const LONG64 written = view->WritePosition;
		if (rate <= 0 || written <= 0) {
			bSynced = false;
			return false;
		}
		MemoryBarrier();

		double step = static_cast<double>(rate) / FrameRate;
		const double needed = step * Frames + 2 + Resampler::Lobes * (step > 1.0 ? step : 1.0) + 1;
		const double oldest = static_cast<double>(written) - RelayBuffer::Capacity * 0.9;

		double desired = 0;
		const ClockState clock = ClockedPosition(view, rate, desired);
		if (clock == ClockState::Paused) {
			bSynced = false;
			return false;
		}
		if (clock == ClockState::Playing) {
			if (desired > written - needed)
				desired = written - needed;
			if (desired < oldest) {
				bSynced = false;
				return false;
			}
			const double drift = desired - ReadPosition;
			if (!bSynced || rate != SyncedRate || drift > 0.08 * rate || drift < -0.08 * rate) {
				ReadPosition = desired;
				SyncedRate = rate;
				bSynced = true;
			}
			else {
				const double correction = drift / (2.0 * rate);
				step *= 1.0 + (correction > 0.002 ? 0.002 : (correction < -0.002 ? -0.002 : correction));
			}
		}
		else {
			const double target = 0.3 * rate;
			const double maxLag = 2.0 * rate;
			if (!bSynced || rate != SyncedRate || ReadPosition > written || written - ReadPosition > maxLag || ReadPosition < oldest) {
				ReadPosition = static_cast<double>(written) - target;
				if (ReadPosition < 0)
					ReadPosition = 0;
				SyncedRate = rate;
				bSynced = true;
			}
		}
		if (static_cast<double>(written) - ReadPosition < needed)
			return false;

		const float* samples = RelayBuffer::Samples(view);
		const double scale = step > 1.0 ? step : 1.0;
		double position = ReadPosition;
		for (UINT32 f = 0; f < Frames; f++) {
			float left = 0;
			float right = 0;
			Resampler::Sample(samples, position, scale, left, right);

			float* frame = Target + static_cast<size_t>(f) * Channels;
			if (Channels == 1) {
				frame[0] = Limit(frame[0] + (left + right) * 0.5f);
			}
			else {
				frame[0] = Limit(frame[0] + left);
				frame[1] = Limit(frame[1] + right);
				for (UINT32 c = 2; c < Channels; c++)
					frame[c] = Limit(frame[c] + (left + right) * 0.5f);
			}
			position += step;
		}
		ReadPosition = position;
		return true;
	}

	static float Limit(float Value)
	{
		constexpr float knee = 0.8f;
		const float magnitude = std::fabs(Value);
		if (magnitude <= knee)
			return Value;
		const float limited = knee + (1.f - knee) * std::tanh((magnitude - knee) / (1.f - knee));
		return Value < 0 ? -limited : limited;
	}

	InnerImpl InnerUnknown;
	IUnknown* Outer;
	std::atomic<ULONG> Refs = 1;

	UINT32 Channels = 2;
	float FrameRate = 48000.f;
	bool bLocked = false;

	HANDLE Mapping = nullptr;
	RelayBuffer::Header* View = nullptr;
	bool bSynced = false;
	LONG SyncedRate = 0;
	double QpcFrequency = 1.0;
	double ReadPosition = 0;
};

class RelayFactory final : public IClassFactory
{
public:
	STDMETHODIMP QueryInterface(REFIID Iid, void** Object) override
	{
		if (!Object)
			return E_POINTER;
		if (Iid == __uuidof(IUnknown) || Iid == __uuidof(IClassFactory)) {
			*Object = static_cast<IClassFactory*>(this);
			AddRef();
			return S_OK;
		}
		*Object = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef() override
	{
		RelayModule::Locks++;
		return 2;
	}

	STDMETHODIMP_(ULONG) Release() override
	{
		RelayModule::Locks--;
		return 1;
	}

	STDMETHODIMP CreateInstance(IUnknown* Outer, REFIID Iid, void** Object) override
	{
		if (!Object)
			return E_POINTER;
		*Object = nullptr;
		if (Outer && Iid != __uuidof(IUnknown))
			return CLASS_E_NOAGGREGATION;
		RelayApo* apo = new (std::nothrow) RelayApo(Outer);
		if (!apo)
			return E_OUTOFMEMORY;
		const HRESULT hr = apo->Inner()->QueryInterface(Iid, Object);
		apo->Inner()->Release();
		return hr;
	}

	STDMETHODIMP LockServer(BOOL Lock) override
	{
		if (Lock)
			RelayModule::Locks++;
		else
			RelayModule::Locks--;
		return S_OK;
	}
};

static RelayFactory Factory;

STDAPI DllGetClassObject(REFCLSID Clsid, REFIID Iid, void** Object)
{
	if (!Object)
		return E_POINTER;
	*Object = nullptr;
	if (Clsid != RelayModule::Clsid)
		return CLASS_E_CLASSNOTAVAILABLE;
	return Factory.QueryInterface(Iid, Object);
}

STDAPI DllCanUnloadNow()
{
	return (RelayModule::Objects == 0 && RelayModule::Locks == 0) ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID)
{
	if (Reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(Instance);
	return TRUE;
}
