#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic> 
#include <filesystem>

const UINT bufferTimeMs = 33; // 버퍼 대기 시간 (ms)



// 전역 변수 설정
HHOOK hMouseHook = NULL;
const ULONG_PTR INJECTED_FLAG = 0xDEADBEEF;

// 버퍼 및 스레드 동기화 관련 변수
std::vector<int> wheelBuffer;
std::mutex bufferMutex;          // 메인 스레드(훅)와 타이머 스레드 간의 데이터 충돌 방지
ULONGLONG firstTick = 0;         // 첫 입력이 들어온 시간
std::atomic<bool> isRunning(true); // 프로그램 실행 상태
int lastDelta = 120;

bool RegisterAutoRun(const std::wstring& appName, const std::wstring& appPath) {
    HKEY hKey;
    // 레지스트리 경로: Run
    const wchar_t* regPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPath, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        // 레지스트리 값 설정
        LONG res = RegSetValueExW(hKey, appName.c_str(), 0, REG_SZ,
            (BYTE*)appPath.c_str(), (DWORD)(appPath.size() + 1) * sizeof(wchar_t));

        RegCloseKey(hKey);
        return res == ERROR_SUCCESS;
    }
    return false;
}

// 버퍼의 입력을 분석해 일괄 전송 (Flush 역할)
void FlushWheelEvents(const std::vector<int>& buffer) {
    int upCount = 0;
    int downCount = 0;

    for (int delta : buffer) {
        if (delta > 0) upCount++;
        else if (delta < 0) downCount++;
    }

    // 기존 코드의 다수결 논리 오류 수정 (동점일 경우 첫 입력 방향을 따름)
    int majorityDelta = 0;
    if (upCount > downCount) {
        majorityDelta = WHEEL_DELTA;
    }
    else if (downCount > upCount) {
        majorityDelta = -WHEEL_DELTA;
    }
    else {
        if (buffer[0] > 0 && lastDelta == WHEEL_DELTA)
            majorityDelta = WHEEL_DELTA;
        else if (buffer[0] < 0 && lastDelta == -WHEEL_DELTA)
            majorityDelta = -WHEEL_DELTA;
        else
            majorityDelta = 0;
    }

    if (buffer.size() == 1) {
        if (lastDelta != majorityDelta) {
            lastDelta = majorityDelta;
            return;
        }
    }

    lastDelta = majorityDelta;

    size_t totalEvents = buffer.size();
    std::vector<INPUT> inputs(totalEvents);
    for (size_t i = 0; i < totalEvents; ++i) {
        inputs[i].type = INPUT_MOUSE;
        inputs[i].mi.dx = 0;
        inputs[i].mi.dy = 0;
        inputs[i].mi.mouseData = majorityDelta;
        inputs[i].mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputs[i].mi.time = 0;
        inputs[i].mi.dwExtraInfo = INJECTED_FLAG;
    }

    SendInput(static_cast<UINT>(totalEvents), inputs.data(), sizeof(INPUT));
 
}

// 전용 타이머 스레드 함수
void TimerThreadFunc() {
    while (isRunning) {
        std::vector<int> localBuffer;

        // 뮤텍스 잠금 (안전하게 버퍼 상태와 시간 확인)
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            if (!wheelBuffer.empty() && (GetTickCount64() - firstTick >= bufferTimeMs)) {
                localBuffer = wheelBuffer; // 로컬 버퍼로 복사
                wheelBuffer.clear();       // 전역 버퍼 초기화
            }
        } // 여기서 뮤텍스 자동 잠금 해제

        // 조건이 충족되어 버퍼를 가져왔다면 SendInput 실행 (뮤텍스 밖에서 실행하여 훅 지연 방지)
        if (!localBuffer.empty()) {
            FlushWheelEvents(localBuffer);
        }

        // CPU 점유율이 100%로 치솟는 것을 방지하기 위해 1ms 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); 
    }
}

// 마우스 이벤트 가로채기 (Low Level Mouse Hook)
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT* pMouseStruct = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        if (pMouseStruct != nullptr) {
            // 주입된 이벤트 통과
            if (pMouseStruct->dwExtraInfo == INJECTED_FLAG) {
                int16_t wheelDelta = HIWORD(pMouseStruct->mouseData);
                if (wheelDelta > 0) {
                    //std::cout << "[출력됨] Scrolled Up (보정된 휠)" << std::endl;
                }
                else {
                    //std::cout << "[출력됨] Scrolled Down (보정된 휠)" << std::endl;
                }
                return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
            }

            // 실제 사용자 입력
            int16_t wheelDelta = HIWORD(pMouseStruct->mouseData);

            // 뮤텍스 잠금 후 버퍼에 추가
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                if (wheelBuffer.empty()) {
                    firstTick = GetTickCount64(); // 새로운 연속 입력의 시작 시간 기록
                }
                wheelBuffer.push_back(wheelDelta);
            }

      /*      if (wheelDelta > 0) std::cout << "캡처됨: Up" << std::endl;
            else std::cout << "캡처됨: Down" << std::endl;*/

            return 1; // 메시지 차단
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

int main() {
    std::wstring name = L"wheelfilter"; 
    std::wstring path = std::filesystem::current_path();

    if (!RegisterAutoRun(name, path)) {
        return 0;
    }

    ShowWindow(GetConsoleWindow(), SW_HIDE);


    // 타이머를 처리할 독립 스레드 생성 및 백그라운드 분리
    std::thread timerThread(TimerThreadFunc);
    timerThread.detach();

    // 전역 로우레벨 마우스 훅 설치 (오타 수정: WHㅊ_MOUSE_LL -> WH_MOUSE_LL)
    hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    if (hMouseHook == NULL) {
        std::cerr << "훅 설치 실패! 오류 코드: " << GetLastError() << std::endl;
        return 1;
    }

    // 메시지 루프
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 종료 처리
    isRunning = false;
    UnhookWindowsHookEx(hMouseHook);

    return 0;
}