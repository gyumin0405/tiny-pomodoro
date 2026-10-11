# 🍅 tiny-pomodoro

화면 구석에 조그맣게 떠 있는 Windows용 초경량 포모도로 타이머.

순수 Win32 C로 작성했고 C 런타임 없이 빌드합니다. 실행 중에도 **메모리 약 0.4MB, CPU는 거의 0**입니다.

## 기능

- 화면 왼쪽 아래에 작게 표시, 항상 위
- 마우스를 올리지 않으면 75% 투명해지고, 올리면 또렷하게
- 드래그로 이동 (위치 기억)
- 🔥 공부 / ☕ 휴식, **25분/5분** 또는 **50분/10분** 모드
- 시간이 끝나도 자동으로 넘어가지 않음. 알림음과 함께 깜빡이고, **클릭해야** 다음 단계로 전환

## 사용법

| 동작 | 결과 |
|---|---|
| 왼쪽 클릭 | 시작 / 일시정지 (시간 종료 후엔 공부↔휴식 전환하고 바로 시작) |
| 드래그 | 위치 이동 |
| 오른쪽 클릭 | 모드 선택, 수동 전환, 리셋, 위치 초기화, 종료 |

설정(모드, 위치)은 `%APPDATA%\Pomodoro\settings.ini`에 저장됩니다.

## 다운로드

[Releases](../../releases)에서 `PomodoroNative.exe`를 받아 실행하면 됩니다. 설치 필요 없음.

## 왜 이렇게 가벼운가

처음엔 C#(WinForms)으로 만들었다가, 실험 삼아 같은 기능과 디자인을 순수 Win32 C로 다시 썼습니다.

| 타이머 실행 중, 30초 측정 | C# 버전 | C 버전 |
|---|---|---|
| CPU 시간 | 312ms | ≤ 15.6ms |
| 메모리 (Private WS) | 6.55MB | 0.43MB |
| 메모리 (전체 WS) | 33.4MB | 1.27MB |
| 스레드 / 핸들 | 7 / 276 | 1 / 84 |

- **폴링 없음**: 마우스 hover는 `TrackMouseEvent`로 이벤트를 받아 처리. 할 일이 없으면 타이머가 하나도 돌지 않음
- **필요할 때만 깨어남**: 표시되는 초가 바뀌는 순간에만 타이머를 걸어 다시 그림
- **컬러 이모지**: GDI는 컬러 이모지를 못 그려서, 미리 렌더링한 48px 이미지를 exe에 내장하고 시작할 때 한 번만 화면 크기로 축소한 뒤 픽셀 버퍼에 직접 알파 블렌딩
- **CRT 없음**: `-nostdlib`로 kernel32/user32/gdi32/dwmapi만 링크

## 빌드

[MinGW-w64](https://www.mingw-w64.org/) gcc가 PATH에 있어야 합니다.

```bat
native\build.bat
```

| 파일 | 설명 |
|---|---|
| `native/pomodoro.c` | 타이머 전체 소스 |
| `native/emoji_data.h` | 내장 이모지 데이터 (`gen_emoji.ps1`로 `emoji/*.png`에서 생성) |
| `make_icon.ps1` | `icon/tomato-sauce.png` → `tomato-sauce.ico` 변환 |

## 라이선스

코드는 [MIT License](LICENSE)를 따릅니다.

## Credits

- 앱 아이콘: <a href="https://www.flaticon.com/free-icons/can" title="can icons">Can icons created by Magnific - Flaticon</a> ([Tomato sauce](https://www.flaticon.com/free-icon/tomato-sauce_904183), Flaticon License). 아이콘은 MIT 라이선스 대상이 아닙니다.
