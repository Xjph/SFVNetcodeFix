#include <Windows.h>
#include <Psapi.h>
#include <stdio.h>
#include <fstream>
#include <time.h>
#include <direct.h>

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

void filename(char* str, int len)
{
	time_t now;
	tm FmtNow;
	char dir[256];

	time(&now);
	localtime_s(&FmtNow, &now);
	//	snprintf(dir, 256, "C:\\sfv-logs-%d", (int)now);
	_mkdir("C:\\sfv-logs");
	snprintf(dir, 256, "C:\\sfv-logs\\current");
	_mkdir(dir);
	strftime(str, len, "c:\\sfv-logs\\current\\log-%F-%H%M%S.txt", &FmtNow);
}

// Called after UInputUnit::UpdateTimestamps
extern "C" void UpdateTimestampsHook(UInputUnit * Input)
{
	const auto OldTimeBase = Input->TimeBase;
	UpdateTimestampsOrigWrapper(Input);

	static char DBGStr[1024];
	static std::ofstream DBGFile; // , BINFile;

	static unsigned int LastPingFrames = 0;
	// Game sets DesiredTimestamp to be less than CurrentTimestamp at round start,
	// but converges it to CurrentTimestamp + 1.
	// Once converged, hold it there during brief mash lag.
	static bool TimestampAdjustAllowed = false;

	// Game hasn't started yet if TimeBase is still updating
	if (Input->TimeBase != OldTimeBase)
	{
		if (DBGFile.is_open())
		{
			DBGFile << "Waiting for TimeBase\n";
			DBGFile.close();
			TimestampAdjustAllowed = false;
			//BINFile.close();
		}
		LastPingFrames = 0;
		return;
	}

	unsigned int Ping;
	if (!GetPing(&Ping))
	{
		LastPingFrames = 0;
		if (DBGFile.is_open())
		{
			DBGFile << "Failed to get ping\n";
			DBGFile.close();
			TimestampAdjustAllowed = false;
			//BINFile.close();
		}
		return;
	}

	if (!DBGFile.is_open() && (Input->TimeBase > 150) && (Input->TimeBase < 500))
	{
		filename(DBGStr, 1024);
		DBGFile.open(DBGStr, std::ofstream::out | std::ofstream::app);
		char* loc = strstr(DBGStr, "log-");
		memcpy_s(loc, 64, "bin", 3);
		loc = strstr(DBGStr, ".txt");
		memcpy_s(loc, 64, ".bin", 4);
		//BINFile.open(DBGStr, std::ofstream::out | std::ofstream::app | std::ofstream::binary);
	}

	int len = sizeof(*Input);
	//BINFile.write((char*)Input, len);
	auto PingFrames = (unsigned int)((float)Ping * 60.f / 1000.f + .5f);
	static int OldRealtimeStamp = 0;
	static int DesiredTimestampCounter = 0;
	static int LastOpponentTimestamp = 0;

	if (TimestampAdjustAllowed && ((Input->DesiredTimestamp + 1) < Input->CurrentTimestamp) && (DesiredTimestampCounter < 4))
	{
		DesiredTimestampCounter++;
		if (DBGFile.is_open())
		{
			snprintf(DBGStr, 1024, "Adjusting desired timestamp from %d to %d\n", Input->DesiredTimestamp, Input->CurrentTimestamp - 1);
			DBGFile << DBGStr;
		}
		Input->DesiredTimestamp = Input->CurrentTimestamp - 1;
	}
	else
	{
		DesiredTimestampCounter = 0;
		TimestampAdjustAllowed = TimestampAdjustAllowed | (Input->DesiredTimestamp > Input->CurrentTimestamp);
	}

	auto ct = Input->CurrentTimestamp % 60;
	if (ct == 0)
	{
		int ts = GetTickCount();
		snprintf(DBGStr, 1024, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d, TimeDiff %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate, ts - OldRealtimeStamp);
		OldRealtimeStamp = ts;
	}
	else if (ct == 1)
	{
		int ts = GetTickCount();
		snprintf(DBGStr, 1024, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d, RealTime %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate, ts);
	}
	else
		snprintf(DBGStr, 1024, "UpdateTimestamps: Ping %d, PingFrames %d, TimeBase %d, CurrentTimestamp %d, OpponentTimestamp %d, DesiredTimestamp %d, MaxFramesAhead %d, FramesToSimulate %d\n",
			Ping, PingFrames, Input->TimeBase, Input->CurrentTimestamp, Input->OpponentTimestamp, Input->DesiredTimestamp, Input->MaxFramesAhead, Input->FramesToSimulate);
	if (DBGFile.is_open())
		DBGFile << DBGStr;

	// Don't hitch from small ping fluctuations
	if (PingFrames == LastPingFrames - 1)
		PingFrames = LastPingFrames;
	else
		LastPingFrames = PingFrames;

	static int LagTime = 0;

	// Don't get farther ahead than normal for compatibility, even with high ping
//	Input->MaxFramesAhead = min(PingFrames + 1, 15);
	/*
	if (Input->CurrentTimestamp < 280)
		LagTime = 0;
	else if (Input->OpponentTimestamp == LastOpponentTimestamp) // Packet loss, opponent lag, or whatever
	{
		LagTime = Input->CurrentTimestamp;
		snprintf(DBGStr, 1024, "Setting lag time %d\n", LagTime);
		DBGFile << DBGStr;
	}
	
	if (Input->CurrentTimestamp < LagTime + 4)
		Input->MaxFramesAhead = 15;
		else
					Input->MaxFramesAhead = PingFrames + 1;
					*/

	if ((Input->CurrentTimestamp & 0x7f) < 16)
		Input->MaxFramesAhead = PingFrames + 1;
	else
		Input->MaxFramesAhead = 15;

	LastOpponentTimestamp = Input->OpponentTimestamp;

	if (false && (Input->CurrentTimestamp >= Input->OpponentTimestamp + Input->MaxFramesAhead))
	{
		// Don't speed up after waiting for the opponent if ping increases
		if (DBGFile.is_open())
			DBGFile << "Reducing TimeBase\n";
		Input->TimeBase--;
	}

	// Never get ahead of where we should be based on real time
	const auto TargetTimestamp = min(Input->DesiredTimestamp, Input->OpponentTimestamp + Input->MaxFramesAhead);
	if (false && (Input->CurrentTimestamp < TargetTimestamp))
	{
		// Speed up to correct for hitch
//		Input->FramesToSimulate = TargetTimestamp - Input->CurrentTimestamp;
		snprintf(DBGStr, 1024, "Updating FramesToSimulate: %d, would be %d\n", Input->FramesToSimulate,
			(Input->OpponentTimestamp + Input->MaxFramesAhead - Input->CurrentTimestamp));
		if (DBGFile.is_open())
			DBGFile << DBGStr;
	}

	//Input->FramesToSimulate = max(PingFrames - 1, 1);
	//if (DBGFile.is_open())
		//DBGFile.flush();
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