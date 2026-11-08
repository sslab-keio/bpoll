#include "trace.h"

__attribute__((noipa, used))
static void usched_trace_pick_next_candidate(pid_t self_pid, int self_idx,
	pid_t candidate_pid, int candidate_idx, int has_packet, int chosen)
{
	asm volatile("nop" :: "r"(self_pid), "r"(self_idx), "r"(candidate_pid),
		"r"(candidate_idx), "r"(has_packet), "r"(chosen) : "memory");
}

__attribute__((noipa, used))
static void usched_trace_pre_priority_switch(pid_t self_pid, int self_idx,
	pid_t next_pid, int next_idx)
{
	asm volatile("nop" :: "r"(self_pid), "r"(self_idx), "r"(next_pid),
		"r"(next_idx) : "memory");
}

__attribute__((noipa, used))
static void usched_trace_post_priority_switch(pid_t self_pid, int self_idx)
{
	asm volatile("nop" :: "r"(self_pid), "r"(self_idx) : "memory");
}

