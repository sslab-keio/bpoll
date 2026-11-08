#ifndef __USCHED_TRACE_H__
#define __USCHED_TRACE_H__

#ifdef TRACE
#define TRACE_PICK_NEXT_CANDIDATE(...) usched_trace_pick_next_candidate(__VA_ARGS__)
#define TRACE_PRE_PRIORITY_SWITCH(...) usched_trace_pre_priority_switch(__VA_ARGS__)
#define TRACE_POST_PRIORITY_SWITCH(...) usched_trace_post_priority_switch(__VA_ARGS__)
#else
#define TRACE_PICK_NEXT_CANDIDATE(...) ((void)0)
#define TRACE_PRE_PRIORITY_SWITCH(...) ((void)0)
#define TRACE_POST_PRIORITY_SWITCH(...) ((void)0)
#endif

#endif // __USCHED_TRACE_H__

