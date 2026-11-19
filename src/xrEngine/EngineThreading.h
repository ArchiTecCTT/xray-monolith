#pragma once

namespace XRay::Engine
{
	void PreRenderThread();
    void PreRenderPostTransformsThread();
	void CalculateBonesThread();
	void CalculateBonesTask(); // the same as a task that may find the main thread has already done it
	void GameThread();
}
