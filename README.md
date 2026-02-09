# 🔍 mini-grep

멀티스레드 기반 고속 파일 검색기 (Thread Pool + Producer-Consumer Pattern)

> Multithreaded file search tool - 2.16x faster with Thread Pool pattern

## 🎯 Quick Start
```bash
# 빌드
gcc mini-grep.c -o mini-grep -pthread
gcc single-mini-grep.c -o single-mini-grep

# 실행
./mini-grep [경로] [검색어]
./single-mini-grep [경로] [검색어]

# 예시
./mini-grep /home/pi TODO
```

## ⚡ Performance

**40,127개 파일 기준 (Raspberry Pi 5, 4 cores)**

| 버전 | 소요 시간 | 성능 |
|------|----------|------|
| Single Thread | 0.317초 | 기준 |
| **Multi Thread** | **0.133초** | **2.38배 빠름** |

## 📊 실행 결과

### Multi-thread (8 workers)
```bash
$ ./mini-grep /home/pi TODO
=== 멀티스레드 파일 검색기 ===
검색 경로: /home/pi
검색 키워드: "TODO"
스레드 개수: 8

📁 파일 탐색 + 검색 중...

[Thread 5] 매칭: /home/pi/project/example.c
  크기: 3184 bytes
  수정: 2026-02-03 10:19:32
    13:  * TODO: This filter does NOT block socketcall()

[Thread 3] 매칭: /home/pi/project/main.c
  크기: 9965 bytes
  수정: 2026-02-05 17:08:12
   250:         printf("예시: %s /home/pi/project \"TODO\"\n", argv[0]);

========================================
검색 완료!
총 40129개 파일 스캔, 35개 파일에서 매칭
소요 시간: 0.133초
========================================
```

### Single-thread
```bash
$ ./single-mini-grep /home/pi TODO
=== 싱글스레드 파일 검색기 ===
검색 경로: /home/pi
검색 키워드: "TODO"

📁 파일 탐색 + 검색 중...

매칭: /home/pi/project/example.c
  크기: 3184 bytes
  수정: 2026-02-03 10:19:32
    12:  * TODO: This filter does NOT block socketcall()

========================================
검색 완료!
총 40127개 파일 스캔, 33개 파일에서 매칭
소요 시간: 0.317초
========================================
```

## 🏗️ 핵심 구현

### 1. Thread Pool (Producer-Consumer Pattern)
```
Main Thread (Producer)
    └─> Task Queue
          ├─> Worker Thread 1
          ├─> Worker Thread 2
          ├─> Worker Thread 3
          ...
          └─> Worker Thread 8
```

### 2. 동적 Queue (자동 확장)
```c
typedef struct {
    char **buf;            // 동적 파일 경로 배열
    size_t cap;            // 버퍼 용량 (자동 확장)
    size_t head;           // pop 위치
    size_t tail;           // push 위치
    size_t count;          // 현재 작업 수
    int scan_done;         // 탐색 완료 플래그

    pthread_mutex_t lock;
    pthread_cond_t  cond;
} TaskQueue;
```

- **FIFO 방식** 작업 분배
- 용량 부족 시 **자동 2배 확장**
- **Mutex**로 동시 접근 제어

### 3. Worker Thread (Consumer)
```c
void* worker_thread(void* arg) {
    while (1) {
        pthread_mutex_lock(&q->lock);

        // 작업이 없으면 대기
        while (q->count == 0 && !q->scan_done) {
            pthread_cond_wait(&q->cond, &q->lock);  // CPU 낭비 방지
        }

        // 작업 가져오기
        char* filepath = queue_pop(q);
        pthread_mutex_unlock(&q->lock);

        if (filepath) {
            search_file(filepath, keyword);  // 병렬 검색
            free(filepath);
        }
    }
}
```

**핵심 포인트:**
- **Condition Variable**로 작업 대기 (busy-waiting 방지)
- **Lock 해제 후 검색** → 병렬 처리 최대화
- 먼저 끝난 스레드가 다음 작업 가져감

### 4. 키워드 강조 출력
```c
static void print_line_with_highlight(const char *line, const char *keyword) {
    // 키워드를 빨간색으로 강조
    printf("%s%s%s", COLOR_RED, keyword, COLOR_RESET);
}
```

- ANSI 색상 코드 사용 (`\033[1;31m`)
- grep 스타일 출력

## 🛠️ 기술 스택

- **C (POSIX Threads)**
- **pthread library** (mutex, condition variable)
- **디렉터리 재귀 탐색** (dirent.h)
- **파일 메타데이터** (sys/stat.h)

## 📈 성능 분석

### 왜 2.38배인가? (이론상 4배가 아닌 이유)

1. **동기화 오버헤드**
   - Mutex lock/unlock 비용
   - Condition variable 신호 비용

2. **파일 I/O 경합**
   - 디스크 I/O는 병렬화 한계 존재
   - 4개 코어가 동시에 같은 디스크 접근

3. **작업 분배 비용**
   - Queue push/pop 오버헤드
   - 메모리 할당/해제 비용

**→ 실제 성능은 Amdahl's Law에 따라 제한됨**

## 📝 블로그 포스트

자세한 설명: [Velog - 멀티스레드 파일 검색기](https://velog.io/@your-blog)

## 📄 License

MIT License

## 🏷️ Topics

`multithreading` `thread-pool` `c-programming` `pthread` `file-search` `parallel-processing` `producer-consumer`
