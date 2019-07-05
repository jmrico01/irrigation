#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BCM2708_PERIPHERAL_BASE 0x20000000
#define GPIO_BASE (BCM2708_PERIPHERAL_BASE + 0x200000)

#define BLOCK_SIZE (4 * 1024)

#define GPIO_SET(gpio)   *((gpio).addr + 7)
#define GPIO_CLEAR(gpio) *((gpio).addr + 10)

#define S_TO_NS(s)   ((s)  * 1000000000L)
#define MS_TO_NS(ms) ((ms) * 1000000L)

#define TRIGGER_TIMES_MAX 16

struct Peripheral {
	int mem_fd;
	void* map;
	volatile unsigned int* addr;
};

struct Time {
	int hour, min, sec;
};

struct PumpData {
	int pin;
	unsigned int numTriggerTimes;
	Time triggerTimes[TRIGGER_TIMES_MAX];
	unsigned int durationMs;
	
	unsigned int elapsedMs;
	bool enabled;
};

bool MapPeripheral(Peripheral* p, unsigned long offset)
{
	if ((p->mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
		printf("Failed to open /dev/mem\n");
		return false;
	}
	
	p->map = mmap(
		NULL,
		BLOCK_SIZE,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		p->mem_fd,
		offset
	);
	if (p->map == MAP_FAILED) {
		close(p->mem_fd);
		printf("Failed to map memory\n");
		return false;
	}
	
	p->addr = (volatile unsigned int*)p->map;

	return true;
}

void UnmapPeripheral(Peripheral* p)
{
	munmap(p->map, BLOCK_SIZE);
	close(p->mem_fd);
}

inline void SetInputPin(const Peripheral& p, int pin)
{
	volatile unsigned int* pinGroupAddr = p.addr + pin / 10;
	*pinGroupAddr &= ~(7 << ((pin % 10) * 3));
}

inline void SetOutputPin(const Peripheral& p, int pin)
{
	volatile unsigned int* pinGroupAddr = p.addr + pin / 10;
	*pinGroupAddr &= ~(7 << ((pin % 10) * 3));
	*pinGroupAddr |= (1 << ((pin % 10) * 3));
}

inline void SetPin(const Peripheral& p, int pin)
{
	*(p.addr + 7) = 1 << pin;
}

inline void ClearPin(const Peripheral& p, int pin)
{
	*(p.addr + 10) = 1 << pin;
}

inline bool ReadPin(const Peripheral& p, int pin)
{
	return *(p.addr + 13) & (1 << pin);
}

void GetLocalTime(Time* t)
{
	time_t rawTime;
	time(&rawTime);
	tm* localTime = localtime(&rawTime);
	t->hour = localTime->tm_hour;
	t->min = localTime->tm_min;
	t->sec = localTime->tm_sec;
}

bool IsTimeInRange(const Time& t, const Time& tStart, const Time& tEnd)
{
	if (t.hour == tStart.hour && t.min == tStart.min && t.sec == tStart.sec) {
		return true;
	}
	if (t.hour == tEnd.hour && t.min == tEnd.min && t.sec == tEnd.sec) {
		return true;
	}
	
	// some more complicated stuff, but as long as we update + query
	// the time often enough (few times per second) should be oK
	return false;
}

// This program must be run as root, because we are opening /dev/mem
int main(int argc, char* argv[])
{
	const int PIN_EXIT = 8;
	const int PIN_PING_IN = 25;
	const int PIN_PING_OUT = 7;
	
	const timespec UPDATE_INTERVAL = {
		0,            // seconds
		MS_TO_NS(500) // nanoseconds
	};
	
	const int NUM_PUMPS = 2;
	PumpData pumpData[NUM_PUMPS];
	
	pumpData[0].pin = 22;
	pumpData[0].numTriggerTimes = 2;
	pumpData[0].triggerTimes[0].hour = 9;
	pumpData[0].triggerTimes[0].min = 0;
	pumpData[0].triggerTimes[0].sec = 0;
	pumpData[0].triggerTimes[1].hour = 21;
	pumpData[0].triggerTimes[1].min = 0;
	pumpData[0].triggerTimes[1].sec = 0;
	pumpData[0].durationMs = 10 * 1000;
	
	pumpData[1].pin = 27;
	pumpData[1].numTriggerTimes = 3;
	pumpData[1].triggerTimes[0].hour = 19;
	pumpData[1].triggerTimes[0].min = 6;
	pumpData[1].triggerTimes[0].sec = 0;
	pumpData[1].triggerTimes[1].hour = 19;
	pumpData[1].triggerTimes[1].min = 6;
	pumpData[1].triggerTimes[1].sec = 10;
	pumpData[1].triggerTimes[2].hour = 19;
	pumpData[1].triggerTimes[2].min = 6;
	pumpData[1].triggerTimes[2].sec = 20;
	pumpData[1].durationMs = 5 * 1000;
	
	for (int i = 0; i < NUM_PUMPS; i++) {
		pumpData[i].enabled = false;
	}
	
	Peripheral p;
	if (!MapPeripheral(&p, GPIO_BASE)) {
		printf("Failed to map peripheral\n");
		return 1;
	}
	
	SetInputPin(p, PIN_EXIT);
	SetInputPin(p, PIN_PING_IN);
	SetOutputPin(p, PIN_PING_OUT);
	ClearPin(p, PIN_PING_OUT);

	for (int i = 0; i < NUM_PUMPS; i++) {
		SetOutputPin(p, pumpData[i].pin);
		ClearPin(p, pumpData[i].pin);
	}
	
	Time t, tPrev;
	timespec ts, tsPrev;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	
	while (true) {
		if (ReadPin(p, PIN_EXIT)) {
			printf("Exit signal\n");
			break;
		}
		
		if (ReadPin(p, PIN_PING_IN)) {
			SetPin(p, PIN_PING_OUT);
		}
		else {
			ClearPin(p, PIN_PING_OUT);
		}
		
		tPrev = t;
		GetLocalTime(&t);
		
		tsPrev = ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		timespec tsDiff;
		tsDiff.tv_sec = ts.tv_sec - tsPrev.tv_sec;
		tsDiff.tv_nsec = ts.tv_nsec - tsPrev.tv_nsec;
		if (tsDiff.tv_nsec < 0) {
			tsDiff.tv_sec -= 1;
			tsDiff.tv_nsec += 1000000000L;
		}
		
		for (int i = 0; i < NUM_PUMPS; i++) {
			if (pumpData[i].enabled) {
				pumpData[i].elapsedMs += tsDiff.tv_sec * 1000;
				pumpData[i].elapsedMs += tsDiff.tv_nsec / 1000000;
				if (pumpData[i].elapsedMs >= pumpData[i].durationMs) {
					pumpData[i].enabled = false;
					ClearPin(p, pumpData[i].pin);
					printf("Pump %d turned OFF at %02d:%02d:%02d (ran for %d ms)\n",
						i, t.hour, t.min, t.sec, pumpData[i].elapsedMs);
				}
			}
			else {
				for (int j = 0; j < pumpData[i].numTriggerTimes; j++) {
					if (IsTimeInRange(pumpData[i].triggerTimes[j], tPrev, t)) {
						pumpData[i].enabled = true;
						pumpData[i].elapsedMs = 0;
						SetPin(p, pumpData[i].pin);
						printf("Pump %d turned  ON at %02d:%02d:%02d\n",
							i, t.hour, t.min, t.sec);
						break;
					}
				}
			}
		}
		
		nanosleep(&UPDATE_INTERVAL, NULL);
	}
	
	ClearPin(p, PIN_PING_OUT);
	
	for (int i = 0; i < NUM_PUMPS; i++) {
		ClearPin(p, pumpData[i].pin);
	}
	
	UnmapPeripheral(&p);
	
	return 0;
}
