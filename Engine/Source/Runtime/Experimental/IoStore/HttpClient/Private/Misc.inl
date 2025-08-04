// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

namespace UE::IoStore::HTTP
{

// {{{1 trace ..................................................................

////////////////////////////////////////////////////////////////////////////////
enum class ETrace
{
	LoopCreate,
	LoopTick,
	LoopDestroy,
	ActivityCreate,
	ActivityDestroy,
	SocketCreate,
	SocketDestroy,
	RequestBegin,
	StateChange,
	Wait,
	Unwait,
	Connect,
	Send,
	Recv,
	StartWork,
};

////////////////////////////////////////////////////////////////////////////////
#if UE_TRACE_ENABLED

static void Trace(const struct FActivity*, ETrace, uint32);
static void Trace(UPTRINT, ETrace Action, const class FOutcome* =nullptr);

static void Trace(ETrace Action)					{}
static void Trace(const void*, ETrace Action, ...)	{}

////////////////////////////////////////////////////////////////////////////////
IOSTOREHTTPCLIENT_API const void* GetIaxTraceChannel()
{
	UE_TRACE_CHANNEL(Iax);
	return &Iax;
}

#else

static void							Trace(...)				{}
IOSTOREHTTPCLIENT_API const void*	GetIaxTraceChannel()	{ return nullptr; }

#endif // UE_TRACE_ENABLED



// {{{1 misc ...................................................................

////////////////////////////////////////////////////////////////////////////////
#define IAS_CVAR(Type, Name, Default, Desc, ...) \
	Type G##Name = Default; \
	static FAutoConsoleVariableRef CVar_Ias##Name( \
		TEXT("ias.Http" #Name), \
		G##Name, \
		TEXT(Desc) \
		__VA_ARGS__ \
	)

////////////////////////////////////////////////////////////////////////////////
static IAS_CVAR(int32, RecvWorkThresholdKiB,80,		"Threshold of data remaining at which next request is sent (in KiB)");
static IAS_CVAR(int32, IdleMs,				50'000,	"Time in milliseconds to close idle connections or fail waits");

////////////////////////////////////////////////////////////////////////////////
class FOutcome
{
public:
	static FOutcome Ok(uint32 Result=0);
	static FOutcome Waiting();
	static FOutcome Error(const char* Message, int32 Code=-1);
	static FOutcome None()				{ return Error(""); }
	bool			IsError() const		{ return Message < 0x8000'0000'0000; }
	bool			IsWaiting() const	{ return Tag == WaitTag; }
	bool			IsOk() const		{ return Tag == OkTag; }
	FAnsiStringView GetMessage() const	{ check(IsError()); return (const char*)(Message); }
	int32			GetErrorCode() const{ check(IsError()); return int32(Code); }
	uint32			GetResult() const	{ check(IsOk()); return Result; }

private:
					FOutcome() = default;

	static uint32 const OkTag	= 0x0000'8000;
	static uint32 const WaitTag = 0x0001'8000;

	union {
		struct {
			UPTRINT	Message : 48;
			PTRINT	Code	: 16;
		};
		struct {
			uint32	Result;
			uint32	Tag;
		};
	};
};
static_assert(sizeof(FOutcome) == sizeof(void*));

////////////////////////////////////////////////////////////////////////////////
FOutcome FOutcome::Ok(uint32 Result)
{
	FOutcome Outcome;
	Outcome.Tag = OkTag;
	Outcome.Result = Result;
	return Outcome;
}

////////////////////////////////////////////////////////////////////////////////
FOutcome FOutcome::Waiting()
{
	FOutcome Outcome;
	Outcome.Tag = WaitTag;
	return Outcome;
}

////////////////////////////////////////////////////////////////////////////////
FOutcome FOutcome::Error(const char* Message, int32 Code)
{
	check(Message != nullptr);
	check(Code <= 0xffff && Code >= -0xffff);

	FOutcome Outcome;
	Outcome.Message = UPTRINT(Message);
	Outcome.Code = int16(Code);
	return Outcome;
}



////////////////////////////////////////////////////////////////////////////////
template <typename LambdaType>
static void EnumerateHeaders(FAnsiStringView Headers, LambdaType&& Lambda)
{
	// NB. here we are assuming that we will be dealing with servers that will
	// not be returning headers with "obsolete line folding".

	auto IsOws = [] (int32 c) { return (c == ' ') | (c == '\t'); };

	const char* Cursor = Headers.GetData();
	const char* End = Cursor + Headers.Len();
	do
	{
		int32 ColonIndex = 0;
		for (; Cursor + ColonIndex < End; ++ColonIndex)
		{
			if (Cursor[ColonIndex] == ':')
			{
				break;
			}
		}

		Cursor += ColonIndex;

		const char* Right = Cursor + 1;
		while (Right < End)
		{
			if (Right[0] != '\r' || Right[1] != '\n')
			{
				Right += 1 + (Right[1] != '\r');
				continue;
			}

			FAnsiStringView Name(Cursor - ColonIndex, ColonIndex);

			const char* Left = Cursor + 1;
			for (; IsOws(Left[0]); ++Left);

			Cursor = Right;
			for (; Cursor > Left + 1 && IsOws(Cursor[-1]); --Cursor);

			FAnsiStringView Value (Left, int32(ptrdiff_t(Cursor - Left)));

			if (!Lambda(Name, Value))
			{
				Right = End;
			}

			break;
		}

		Cursor = Right + 2;
	} while (Cursor < End);
}

////////////////////////////////////////////////////////////////////////////////
static int32 FindMessageTerminal(const char* Data, uint32 Length)
{
	for (uint32 i = 4; i <= Length; ++i)
	{
		uint32 Candidate;
		::memcpy(&Candidate, Data + i - 4, sizeof(Candidate));
		if (Candidate == 0x0a0d0a0d)
		{
			return i;
		}

		i += (Data[i - 1] > 0x0d) ? 3 : 0;
	}

	return -1;
}

////////////////////////////////////////////////////////////////////////////////
template <uint32 Base=10>
static int64 CrudeToInt(FAnsiStringView View)
{
	static_assert(Base == 10 || Base == 16);

	// FCStringAnsi::* is not used to mitigate any locale hiccups. By
	// initialising 'Value' with MSB set we can detect cases where View did not
	// start with digits. This works as we won't be using this on huge numbers.
	int64 Value = 0x8000'0000'0000'0000ll;
	for (int32 c : View)
	{
		uint32 Digit = c - '0';
		if (Digit > 9u)
		{
			if (Base != 16)
			{
				break;
			}

			Digit = (c | 0x20) - 'a';
			if (Digit > uint32('f' - 'a'))
			{
				break;
			}
			Digit += 10;
		}
		Value *= Base;
		Value += Digit;
	}
	return Value;
};

////////////////////////////////////////////////////////////////////////////////
struct FMessageOffsets
{
	uint8	StatusCode;
	uint8	Message;
	uint16	Headers;
};

static int32 ParseMessage(FAnsiStringView Message, FMessageOffsets& Out)
{
	const FAnsiStringView Protocol("HTTP/1.1 ");

	// Check there's enough data
	if (Message.Len() < Protocol.Len() + 1) // "+1" accounts for at least one digit
	{
		return -1;
	}

	const char* Cursor = Message.GetData();

	// Check for the expected protocol
	if (FAnsiStringView(Cursor, 9) != Protocol)
	{
		return -1;
	}
	int32 i = Protocol.Len();

	// Trim left and tightly reject anything adventurous
	for (int n = 32; Cursor[i] == ' ' && i < n; ++i);
	Out.StatusCode = uint8(i);

	// At least one status line digit. (Note to self; expect exactly three)
	for (int n = 32; uint32(Cursor[i] - 0x30) <= 9 && i < n; ++i);
	if (uint32(i - Out.StatusCode - 1) > 32)
	{
		return -1;
	}

	// Trim left
	for (int n = 32; Cursor[i] == ' ' && i < n; ++i);
	Out.Message = uint8(i);

	// Extra conservative length allowance
	if (i > 32)
	{
		return -1;
	}

	// Find \r\n
	for (; Cursor[i] != '\r'; ++i)
	{
		if (i >= 2048)
		{
			return -1;
		}
	}
	if (Cursor[i + 1] != '\n')
	{
		return -1;
	}
	Out.Headers = uint16(i + 2);

	return 1;
}

////////////////////////////////////////////////////////////////////////////////
struct FUrlOffsets
{
	struct Slice
	{
						Slice() = default;
						Slice(int32 l, int32 r) : Left(uint8(l)), Right(uint8(r)) {}
		FAnsiStringView	Get(FAnsiStringView Url) const { return Url.Mid(Left, Right - Left); }
						operator bool () const { return Left > 0; }
		int32			Len() const { return Right - Left; }
		uint8			Left;
		uint8			Right;
	};
	Slice				UserInfo;
	Slice				HostName;
	Slice				Port;
	uint8				Path;
	uint8				SchemeLength;
};

static int32 ParseUrl(FAnsiStringView Url, FUrlOffsets& Out)
{
	if (Url.Len() < 5)
	{
		return -1;
	}

	Out = {};

	const char* Start = Url.GetData();
	const char* Cursor = Start;

	// Scheme
	int32 i = 0;
	for (; i < 5; ++i)
	{
		if (uint32(Cursor[i] - 'a') > uint32('z' - 'a'))
		{
			break;
		}
	}

	Out.SchemeLength = uint8(i);
	FAnsiStringView Scheme = Url.Left(i);
	if (Scheme != "http" && Scheme != "https")
	{
		return -1;
	}

	// Separator and authority
	if (Cursor[i] != ':' || Cursor[i + 1] != '/' || Cursor[i + 2] != '/')
	{
		return -1;
	}
	i += 3;

	struct { int32 c; int32 i; } Seps[2];
	int32 SepCount = 0;
	for (; i < Url.Len(); ++i)
	{
		int32 c = Cursor[i];
		if (c < '-')							break;
		if (c != ':' && c != '@' && c != '/')	continue;
		if (c == '/' || SepCount >= 2)			break;

		if (c == '@' && SepCount)
		{
			SepCount -= (Seps[SepCount - 1].c == ':');
		}
		Seps[SepCount++] = { c, i };
	}

	if (i > 0xff || i <= Scheme.Len() + 3)
	{
		return -1;
	}

	if (i < Url.Len())
	{
		Out.Path = uint8(i);
	}

	Out.HostName = { uint8(Scheme.Len() + 3), uint8(i) };

	switch (SepCount)
	{
	case 0:
		break;

	case 1:
		if (Seps[0].c == ':')
		{
			Out.Port = { Seps[0].i + 1, i };
			Out.HostName.Right = uint8(Seps[0].i);
		}
		else
		{
			Out.UserInfo = { Out.HostName.Left, Seps[0].i };
			Out.HostName.Left += uint8(Seps[0].i + 1);
			Out.HostName.Right += uint8(Seps[0].i + 1);
		}
		break;

	case 2:
		if ((Seps[0].c != '@') | (Seps[1].c != ':'))
		{
			return -1;
		}
		Out.UserInfo = { Out.HostName.Left, Seps[0].i };
		Out.Port.Left = uint8(Seps[1].i + 1);
		Out.Port.Right = Out.HostName.Right;
		Out.HostName.Left = Out.UserInfo.Right + 1;
		Out.HostName.Right = Out.Port.Left - 1;
		break;

	default:
		return -1;
	}

	bool Bad = false;
	Bad |= (Out.HostName.Len() == 0);
	Bad |= bool(Out.UserInfo) & (Out.UserInfo.Len() == 0);

	if (Out.Port.Left)
	{
		Bad |= (Out.Port.Len() == 0);
		for (int32 j = 0, n = Out.Port.Len(); j < n; ++j)
		{
			Bad |= (uint32(Start[Out.Port.Left + j] - '0') > 9);
		}
	}

	return Bad ? -1 : 1;
}



// {{{1 buffer .................................................................

////////////////////////////////////////////////////////////////////////////////
class alignas(16) FBuffer
{
public:
	struct FMutableSection
	{
		char*		Data;
		uint32		Size;
	};

								FBuffer() = default;
								FBuffer(char* InData, uint32 InMax);
								~FBuffer();
	FBuffer&					operator = (FBuffer&& Rhs);
	void						Fix();
	void						Resize(uint32 Size);
	const char*					GetData() const;
	uint32						GetSize() const;
	uint32						GetCapacity() const;
	template <typename T> T*	Alloc(uint32 Count=1);
	FMutableSection				GetMutableFree(uint32 MinSize, uint32 PageSize=256);
	void						AdvanceUsed(uint32 Delta);

private:
	char*						GetDataPtr();
	void						Extend(uint32 AtLeast, uint32 PageSize);
	UPTRINT						Data;
	uint32						Max = 0;
	union
	{
		struct
		{
			uint32				Used : 31;
			uint32				Inline : 1;
		};
		uint32					UsedInline = 0;
	};

private:
								FBuffer(FBuffer&&) = delete;
								FBuffer(const FBuffer&) = delete;
	FBuffer&					operator = (const FBuffer&) = delete;
};

////////////////////////////////////////////////////////////////////////////////
FBuffer::FBuffer(char* InData, uint32 InMax)
: Data(UPTRINT(InData))
, Max(InMax)
{
	Inline = 1;
}

////////////////////////////////////////////////////////////////////////////////
FBuffer::~FBuffer()
{
	if (Data && !Inline)
	{
		FMemory::Free(GetDataPtr());
	}
}

////////////////////////////////////////////////////////////////////////////////
FBuffer& FBuffer::operator = (FBuffer&& Rhs)
{
	Swap(Data, Rhs.Data);
	Swap(Max, Rhs.Max);
	Swap(UsedInline, Rhs.UsedInline);
	return *this;
}

////////////////////////////////////////////////////////////////////////////////
void FBuffer::Fix()
{
	check(Inline);
	Data += Used;
	Max -= Used;
	Used = 0;
}

////////////////////////////////////////////////////////////////////////////////
void FBuffer::Resize(uint32 Size)
{
	check(Size <= Max);
	Used = Size;
}

////////////////////////////////////////////////////////////////////////////////
char* FBuffer::GetDataPtr()
{
	return (char*)Data;
}

////////////////////////////////////////////////////////////////////////////////
const char* FBuffer::GetData() const
{
	return (char*)Data;
}

////////////////////////////////////////////////////////////////////////////////
uint32 FBuffer::GetSize() const
{
	return Used;
}

////////////////////////////////////////////////////////////////////////////////
uint32 FBuffer::GetCapacity() const
{
	return Max;
}

////////////////////////////////////////////////////////////////////////////////
template <typename T>
T* FBuffer::Alloc(uint32 Count)
{
	uint32 AlignBias = uint32(Data) & (alignof(T) - 1);
	if (AlignBias)
	{
		AlignBias = alignof(T) - AlignBias;
	}

	uint32 PotentialUsed = Used + AlignBias + (sizeof(T) * Count);
	if (PotentialUsed > Max)
	{
		Extend(PotentialUsed, 256);
	}

	void* Ret = GetDataPtr() + Used + AlignBias;
	Used = PotentialUsed;
	return (T*)Ret;
}

////////////////////////////////////////////////////////////////////////////////
FBuffer::FMutableSection FBuffer::GetMutableFree(uint32 MinSize, uint32 PageSize)
{
	MinSize = (MinSize == 0 && Used == Max) ? PageSize : MinSize;

	uint32 PotentialUsed = Used + MinSize;
	if (PotentialUsed > Max)
	{
		Extend(PotentialUsed, PageSize);
	}

	return FMutableSection{ GetDataPtr() + Used, Max - Used };
}

////////////////////////////////////////////////////////////////////////////////
void FBuffer::AdvanceUsed(uint32 Delta)
{
	Used += Delta;
	check(Used <= Max);
}

////////////////////////////////////////////////////////////////////////////////
void FBuffer::Extend(uint32 AtLeast, uint32 PageSize)
{
	checkSlow((PageSize - 1 & PageSize) == 0);

	--PageSize;
	Max = (AtLeast + PageSize) & ~PageSize;

	if (!Inline)
	{
		Data = UPTRINT(FMemory::Realloc(GetDataPtr(), Max, alignof(FBuffer)));
		return;
	}

	const char* PrevData = GetDataPtr();
	Data = UPTRINT(FMemory::Malloc(Max, alignof(FBuffer)));
	::memcpy(GetDataPtr(), PrevData, Used);
	Inline = 0;
}



////////////////////////////////////////////////////////////////////////////////
class FMessageBuilder
{
public:
						FMessageBuilder(FBuffer& Ruffeb);
	FMessageBuilder&	operator << (FAnsiStringView Lhs);

private:
	FBuffer&		Buffer;
};

////////////////////////////////////////////////////////////////////////////////
FMessageBuilder::FMessageBuilder(FBuffer& Ruffeb)
: Buffer(Ruffeb)
{
}

////////////////////////////////////////////////////////////////////////////////
FMessageBuilder& FMessageBuilder::operator << (FAnsiStringView Lhs)
{
	uint32 Length = uint32(Lhs.Len());
	FBuffer::FMutableSection Section = Buffer.GetMutableFree(Length);
	::memcpy(Section.Data, Lhs.GetData(), Length);
	Buffer.AdvanceUsed(Length);
	return *this;
}



// {{{1 throttler ..............................................................

////////////////////////////////////////////////////////////////////////////////
static void ThrottleTest(FAnsiStringView);

////////////////////////////////////////////////////////////////////////////////
class FThrottler
{
public:
			FThrottler();
	void	SetLimit(uint32 KiBPerSec);
	int32	GetAllowance();
	void	ReturnUnused(int32 Unused);

private:
	friend	void ThrottleTest(FAnsiStringView);
	int32	GetAllowance(int64 CycleDelta);
	int64	CycleFreq;
	int64	CycleLast = 0;
	int64	CyclePeriod = 0;
	uint32	Limit = 0;

	enum : uint32 {
		LIMITLESS	= MAX_int32,
		SLICES_POW2	= 5,
	};
};

////////////////////////////////////////////////////////////////////////////////
FThrottler::FThrottler()
{
	CycleFreq = int64(1.0 / FPlatformTime::GetSecondsPerCycle64());
	check(CycleFreq >> SLICES_POW2);
}

////////////////////////////////////////////////////////////////////////////////
void FThrottler::SetLimit(uint32 KiBPerSec)
{
	// 512MiB/s might as well be limitless.
	KiBPerSec = (KiBPerSec < (512 << 10)) ? KiBPerSec : 0;
	if (KiBPerSec)
	{
		KiBPerSec = FMath::Max(KiBPerSec, 1u << SLICES_POW2);
	}
	Limit = KiBPerSec << 10;
}

////////////////////////////////////////////////////////////////////////////////
int32 FThrottler::GetAllowance()
{
	int64 Cycle = FPlatformTime::Cycles64();
	int64 CycleDelta = Cycle - CycleLast;
	CycleLast = Cycle;
	return GetAllowance(CycleDelta);
}

////////////////////////////////////////////////////////////////////////////////
int32 FThrottler::GetAllowance(int64 CycleDelta)
{
	if (Limit == 0)
	{
		return LIMITLESS;
	}

	int64 CycleSlice = CycleFreq >> SLICES_POW2;
	CycleDelta = FMath::Min(CycleDelta, CycleSlice);
	CyclePeriod -= CycleDelta;
	if (CyclePeriod > 0)
	{
		return 0 - int32((CyclePeriod * 1000ll) / CycleFreq);
	}
	CyclePeriod += CycleSlice;

	int32 Released = Limit >> SLICES_POW2;
	return Released;
}

////////////////////////////////////////////////////////////////////////////////
void FThrottler::ReturnUnused(int32 Unused)
{
	if (Limit == 0 || Unused == 0)
	{
		return;
	}

	int64 CycleReturn = (CycleFreq * Unused) / Limit;
	CycleLast -= CycleReturn;
}

// }}}

} // namespace UE::IoStore::HTTP
