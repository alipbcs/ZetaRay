#ifndef TAA_H
#define TAA_H

#define TAA_THREAD_GROUP_SIZE_X 8u
#define TAA_THREAD_GROUP_SIZE_Y 8u
#define TAA_THREAD_GROUP_SIZE_Z 1

struct cbTAA
{
	float BlendWeight;
	uint32_t InputDescHeapIdx;
	uint32_t PrevOutputDescHeapIdx;
	uint32_t CurrOutputDescHeapIdx;
	uint32_t TemporalIsValid;
};

#endif