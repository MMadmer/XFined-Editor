#pragma once

struct SProgressTaskInfo
{
	xr_string text;
	xr_string detail;
	float current = 0.f;
	float total = 0.f;
	float fraction = 0.f;
	u64 elapsed_ms = 0;
	u32 depth = 0;
	bool determinate = false;
	bool cancelable = false;
	bool cancel_requested = false;
};

using SProgressTaskInfoVec = xr_vector<SProgressTaskInfo>;

class TUI;

namespace UIProgressCenter
{
void Draw(TUI& ui);
void FormatElapsed(u64 elapsed_ms, xr_string& result);
}
