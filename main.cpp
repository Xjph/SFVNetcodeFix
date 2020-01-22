#include <Windows.h>
#include <Psapi.h>
#ifdef DEBUG
#include <stdio.h>
#include <fstream>
#include <time.h>
#include <direct.h>
#endif

constexpr int LAG_THRESHOLD = 16;
constexpr int MAX_PING = 16;
constexpr int GAME_START_FRAME = 210; // Usually starts around frame 200, but there's some variability so leave a buffer
constexpr int PACKET_HISTORY_SIZE = 32;
constexpr int TS_HOLD_TIME = 4; // Only adjust the desired timestamp for this long before allowing it to recover
constexpr int RESYNC_WINDOW = 10;
constexpr int DBGLEN = 1024;

namespace Proud
{
	class CHostBase {};

	class CRemotePeer : CHostBase
	{
	private:
		char pad154[0x150];
	public:
		unsigned int Ping; // One way latency
	};

	class CNetClient {};

	class CNetClientImpl
	{
	private:
		char pad2B8[0x2B8];
	public:
		struct
		{
			int Id;
			CHostBase* Pointer;
		} *Host;
	private:
		char pad540[0x540 - 0x2B8 - 8];
	public:
		CNetClient Base;
	};
}

class UInputUnit
{
private:
	char pad2180[0x2180];
public:
	unsigned int CurrentTimestamp;
	unsigned int OpponentTimestamp;
private:
	char pad21D8[0x21D8 - 0x2180 - 8];
public:
	unsigned int MaxFramesAhead; // Max frames ahead of opponent (will slow down to reach this)
	unsigned int IsPaused; // Stop simulating the game
private:
	char pad220C[0x220C - 0x21D8 - 8];
public:
	unsigned int TimeBase; // Set at round start, used to offset desired timestamp
private:
	char pad2214[0x2214 - 0x220C - 4];
public:
	unsigned int DesiredTimestamp; // Set based on real time, will speed up to reach this
private:
	char pad2220[0x2220 - 0x2214 - 4];
public:
	unsigned int FramesToSimulate; // Number of frames to simulate, used to speed up/slow down
};

class UnknownNetBullshit
{
private:
	char pad010[0x10];
public:
	Proud::CNetClient* Client;
};

class UnknownNetList
{
private:
	char pad070[0x70];
public:
	UnknownNetBullshit** List;
	UnknownNetBullshit** ListEnd;
};

UnknownNetList* NetList;

extern "C" uintptr_t UpdateTimestampsOrig;
uintptr_t UpdateTimestampsOrig;
extern "C" void UpdateTimestampsOrigWrapper(UInputUnit*);

bool GetPing(unsigned int* Ping)
{
	// Index 1 is opponent
	if (NetList->List == nullptr ||
		NetList->List + 1 >= NetList->ListEnd ||
		NetList->List[1] == nullptr ||
		NetList->List[1]->Client == nullptr)
	{
		return false;
	}

	// dynamic_cast to CNetClientImpl
	const auto* Client = NetList->List[1]->Client;
	const auto* ClientImpl = (Proud::CNetClientImpl*)((char*)Client - offsetof(Proud::CNetClientImpl, Base));

	if (ClientImpl->Host == nullptr || ClientImpl->Host->Pointer == nullptr)
		return false;

	const auto* Peer = (Proud::CRemotePeer*)ClientImpl->Host->Pointer;
	*Ping = Peer->Ping;
	return true;
}

#ifdef DEBUG
void filename(char* str, int len)
{
	time_t now;
	tm FmtNow;
	char dir[256];

	time(&now);
	localtime_s(&FmtNow, &now);
	_mkdir("C:\\sfv-logs");
	snprintf(dir, 256, "C:\\sfv-logs\\current");
	_mkdir(dir);
	strftime(str, len, "c:\\sfv-logs\\current\\log-%F-%H%M%S.txt", &FmtNow);
}
#endif

// Called after UInputUnit::UpdateTimestamps
extern "C" void UpdateTimestampsHook(UInputUnit * Input)
{
	const auto OldTimeBase = Input->TimeBase;
	UpdateTimestampsOrigWrapper(Input);

#ifdef DEBUG
	static char DBGStr[DBGLEN];
	static std::ofstream DBGFile;
#endif
	// Game sets DesiredTimestamp to be less than CurrentTimestamp at round start,
	// but converges it to CurrentTimestamp + 1.
	// Once converged, hold it there during brief mash lag.
	static bool TimestampAdjustAllowed = false;
	static int LagRecord[PACKET_HISTORY_SIZE], PingRecord[PACKET_HISTORY_SIZE]; // auto initialized to all zeroes
	static int PacketRecordPosition;
	static int ResyncTimer, ResyncLevel;

	// Game hasn't started yet if TimeBase is still updating
	if (Input->TimeBase != OldTimeBase)
	{
#ifdef DEBUG
		if (DBGFile.is_open())
		{
			DBGFile << "Waiting for TimeBase\n";
			DBGFile.close();
		}
#endif
		TimestampAdjustAllowed = false;
		ResyncTimer = 0;
		return;
	}

	unsigned int Ping;
	if (!GetPing(&Ping))
	{
#ifdef DEBUG
		if (DBGFile.is_open())
		{
			DBGFile << "Failed to get ping\n";
			DBGFile.close();
		}
#endif
		TimestampAdjustAllowed = false;
		ResyncTimer = 0;
		return;
	}

#ifdef DEBUG
	if (!DBGFile.is_open() && (Input->TimeBase > 150) && (Input->TimeBase < 500))
	{
		filename(DBGStr, DBGLEN);
		DBGFile.open(DBGStr, std::ofstream::out | std::ofstream::app);
		char* loc = strstr(DBGStr, "log-");
		memcpy_s(loc, 64, "bin", 3);
		loc = strstr(DBGStr, ".txt");
		memcpy_s(loc, 64, ".bin", 4);
	}
#endif

	auto PingFrames = (unsigned int)((float)Ping * 60.f / 1000.f + .5f);
	if (PingFrames > MAX_PING) // In some games, the GetPing function returns nonsense - I have no idea why.  Nothing can be done in this case
	{
#ifdef DEBUG
		snprintf(DBGStr, DBGLEN, "Horked up a giant ping of %d\n", PingFrames);
		DBGFile << DBGStr;
#endif
		return;
	}

	static int DesiredTimestampCounter = 0;
	if (TimestampAdjustAllowed && ((Input->DesiredTimestamp + 1) < Input->CurrentTimestamp) && (DesiredTimestampCounter < TS_HOLD_TIME))
	{
		DesiredTimestampCounter++;
#ifdef DEBUG
		if (DBGFile.is_open())
		{
			snprintf(DBGStr, DBGLEN, "Adjusting desired timestamp from %d to %d\n", Input->DesiredTimestamp, Input->CurrentTimestamp - 1);
			DBGFile << DBGStr;
		}
#endif
		Input->DesiredTimestamp = Input->CurrentTimestamp - 1;
	}
	else
	{
		DesiredTimestampCounter = 0;
		TimestampAdjustAllowed = TimestampAdjustAllowed | (Input->DesiredTimestamp > Input->CurrentTimestamp);
	}

#ifdef DEBUG
	static int OldRealtimeStamp = 0;
	auto ct = Input->CurrentTimestamp % 60;
	if (ct == 0)
	{
		int ts = GetTickCount();
		snprintf(DBGStr, DBGLEN, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d, TimeDiff %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate, ts - OldRealtimeStamp);
		OldRealtimeStamp = ts;
	}
	else if (ct == 1)
	{
		int ts = GetTickCount();
		snprintf(DBGStr, DBGLEN, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d, RealTime %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate, ts);
	}
	else
		snprintf(DBGStr, DBGLEN, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate);
	if (DBGFile.is_open())
		DBGFile << DBGStr;
#endif

	if (ResyncTimer > 0) // Resync ordered by previous lag detection
	{
		ResyncTimer--;
		Input->MaxFramesAhead = ResyncLevel;
	}
	else if (Input->CurrentTimestamp == GAME_START_FRAME) // On first monitored frame, initialize the buffers
	{
		memset(LagRecord, 0, PACKET_HISTORY_SIZE * sizeof(int));
		memset(PingRecord, 0, PACKET_HISTORY_SIZE * sizeof(int));
	}
	else if (Input->CurrentTimestamp > GAME_START_FRAME) // Normal monitored frames - record lag stats, then check for lag in effect
	{
		int LagNow = Input->CurrentTimestamp - Input->OpponentTimestamp;
		LagRecord[PacketRecordPosition] = LagNow;
		PingRecord[PacketRecordPosition] = PingFrames;
		if (PacketRecordPosition == PACKET_HISTORY_SIZE - 1)
			PacketRecordPosition = 0;
		else
			PacketRecordPosition++;

		if (Input->CurrentTimestamp > GAME_START_FRAME + PACKET_HISTORY_SIZE)
		{
			if (LagNow > (int)PingFrames + 1) // Buffers full and lag is ominous
			{
#ifdef DEBUG
				DBGFile << "Performing detailed lagalysis... ";
#endif
				int PingHistogram[MAX_PING]; // Frequency of half-ping times with which packets are received
				memset(PingHistogram, 0, MAX_PING * sizeof(int));
				for (int i = 0; i < PACKET_HISTORY_SIZE; i++)
				{
					int ClampedPing = PingRecord[i];
					if (ClampedPing < MAX_PING - 1)
						PingHistogram[PingRecord[i]]++;
					else
						PingHistogram[MAX_PING - 1]++;
				}

				// Find the mode of the ping, rather than the average or recent
				int MostCommonPing = 0, HighPingCount = 0;
				for (int i = 0; i < MAX_PING; i++)
					if (PingHistogram[i] > HighPingCount)
					{
						HighPingCount = PingHistogram[i];
						MostCommonPing = i;
					}
#ifdef DEBUG
				snprintf(DBGStr, DBGLEN, "Ping mode is %d\n", MostCommonPing);
				DBGFile << DBGStr;
#endif

				// Finally determine if there have been enough laggy frames recently to activate the lag compensation
				int LaggyFrames = 0;
				for (int i = 0; i < PACKET_HISTORY_SIZE; i++)
					if (LagRecord[i] > PingRecord[i] + 1)
						LaggyFrames++;
				if (LaggyFrames > LAG_THRESHOLD)
				{
#ifdef DEBUG
					snprintf(DBGStr, DBGLEN, "Activating anti-lag, %d laggy frames detected\n", LaggyFrames);
					DBGFile << DBGStr;
#endif
					ResyncLevel = MostCommonPing;
					Input->MaxFramesAhead = ResyncLevel;
					ResyncTimer = RESYNC_WINDOW;
					memset(LagRecord, 0, PACKET_HISTORY_SIZE * sizeof(int)); // Old lag record might cause false positive after resync complete
				}
			}
			else
				Input->MaxFramesAhead = 15; // Magic number 15 is used by vanilla, works in lag-free case
		}
		else // Start of round behavior
		{
			Input->MaxFramesAhead = PingFrames + 1;
		}
	}
}

bool GetModuleBounds(const char* Name, uintptr_t* Start, uintptr_t* End)
{
	const auto Module = GetModuleHandle(Name);
	if (Module == nullptr)
		return false;

	MODULEINFO Info;
	GetModuleInformation(GetCurrentProcess(), Module, &Info, sizeof(Info));
	*Start = (uintptr_t)(Info.lpBaseOfDll);
	*End = *Start + Info.SizeOfImage;
	return true;
}

uintptr_t Sigscan(const uintptr_t Start, const uintptr_t End, const char* Sig, const char* Mask)
{
	const auto ScanEnd = End - strlen(Mask) + 1;
	for (auto Address = Start; Address < ScanEnd; Address++) {
		for (size_t i = 0;; i++) {
			if (Mask[i] == '\0')
				return Address;
			if (Mask[i] != '?' && Sig[i] != *(char*)(Address + i))
				break;
		}
	}

	return 0;
}

uintptr_t GetRel32(const uintptr_t Address)
{
	return Address + *(int*)(Address)+4;
}

void JmpHook(const uintptr_t Address, void* Target)
{
	constexpr auto PatchSize = 12;

	DWORD OldProtect;
	VirtualProtect((void*)Address, PatchSize, PAGE_EXECUTE_READWRITE, &OldProtect);

	*(WORD*)Address = 0xB848; // mov rax, Target
	*(void**)(Address + 2) = Target;
	*(WORD*)(Address + 10) = 0xE0FF; // jmp rax

	VirtualProtect((void*)Address, PatchSize, OldProtect, &OldProtect);
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, void* Reserved)
{
	if (Reason != DLL_PROCESS_ATTACH)
		return FALSE;

	uintptr_t Start, End;
	if (!GetModuleBounds("StreetFighterV.exe", &Start, &End))
		return FALSE;

	NetList = (UnknownNetList*)GetRel32(Sigscan(Start, End, "\x9B\x00\x00\x00\x83\xC8\x01", "xxxxxxx") + 0xA);

	UpdateTimestampsOrig = Sigscan(Start, End, "\x83\xBB\x04\x22\x00\x00\x00", "xxxxxxx") - 0x29;
	JmpHook(UpdateTimestampsOrig, UpdateTimestampsHook);

	return TRUE;
}