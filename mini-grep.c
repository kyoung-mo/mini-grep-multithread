/**
 * 멀티스레드 파일 검색기 (mini-grep, unlimited queue)
 *
 * 기능:
 * - 디렉터리 재귀 탐색 (Main thread: Producer)
 * - Thread pool (기본 8개 Worker: Consumer)
 * - 동적 Queue (파일 개수 제한 없음에 가깝게)
 * - Mutex + Condition Variable
 * - 키워드 빨간색 강조 (grep 스타일)
 *
 * 빌드:
 *   gcc mini_grep_mt.c -o mini_grep_mt -pthread
 *
 * 실행:
 *   ./mini_grep_mt /path "TODO"
 */

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MAX_PATH
#define MAX_PATH 10000
#endif

#define MAX_THREADS 8

// -------------------- ANSI 색상 코드 --------------------
#define COLOR_RED     "\033[1;31m"
#define COLOR_RESET   "\033[0m"

// -------------------- 전역 통계/출력 락 --------------------
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stat_lock  = PTHREAD_MUTEX_INITIALIZER;

static long long scanned_files = 0;   // 스캔한 "대상 파일" 개수
static long long total_matches = 0;   // 매칭된 "파일" 개수(파일 단위)

// -------------------- 동적 링버퍼 Queue --------------------
typedef struct {
    char **buf;            // 파일 경로 문자열 포인터 배열
    size_t cap;            // 버퍼 용량
    size_t head;           // pop 위치
    size_t tail;           // push 위치
    size_t count;          // 현재 원소 수
    int scan_done;         // producer 종료 플래그

    pthread_mutex_t lock;
    pthread_cond_t  cond;
} TaskQueue;

static void queue_init(TaskQueue *q) {
    q->cap = 1024; // 시작 용량 (필요시 자동 증가)      
    q->buf = (char**)calloc(q->cap, sizeof(char*));
    if (!q->buf) {
        perror("calloc");
        exit(1);
    }
    q->head = q->tail = q->count = 0;
    q->scan_done = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_destroy(TaskQueue *q) {
    // 남아있는 아이템 정리
    for (size_t i = 0; i < q->count; i++) {
        size_t idx = (q->head + i) % q->cap;
        free(q->buf[idx]);
    }
    free(q->buf);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

// cap을 2배로 늘리고 순서를 head부터 재배열
static void queue_grow(TaskQueue *q) {      
    size_t new_cap = q->cap * 2;
    char **new_buf = (char**)calloc(new_cap, sizeof(char*));
    if (!new_buf) {
        perror("calloc(grow)");
        exit(1);
    }

    for (size_t i = 0; i < q->count; i++) {
        size_t idx = (q->head + i) % q->cap;
        new_buf[i] = q->buf[idx];
    }

    free(q->buf);
    q->buf = new_buf;
    q->cap = new_cap;
    q->head = 0;
    q->tail = q->count;
}

// push: 문자열은 strdup 해서 queue가 소유
static void queue_push(TaskQueue *q, const char *path) {
    pthread_mutex_lock(&q->lock);

    if (q->count == q->cap) {
        queue_grow(q);
    }

    q->buf[q->tail] = strdup(path);
    if (!q->buf[q->tail]) {
        perror("strdup");
        pthread_mutex_unlock(&q->lock);
        exit(1);
    }

    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    // 작업 생김 -> 깨우기
    pthread_cond_signal(&q->cond);      // 작업이 생기면 스레드 깨우기 
    pthread_mutex_unlock(&q->lock);
}

// pop: 성공하면 char* 반환(호출자가 free), 없으면 NULL
static char* queue_pop(TaskQueue *q) {
    char *ret = NULL;

    // lock은 worker에서 잡고 들어올 수도 있지만,
    // 여기서는 단순화 위해 pop 내부에서 lock을 잡지 않고,
    // worker가 lock 잡은 상태에서만 호출하도록 설계할 수도 있음.
    // -> 하지만 실수 방지 위해 pop 자체는 lock 없이 쓰지 않도록 "외부에서 lock 잡고 호출"로 통일.
    if (q->count == 0) return NULL;

    ret = q->buf[q->head];
    q->buf[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;      // Queue 기반 작업 분배
    q->count--;

    return ret;
}

// -------------------- 키워드 강조 출력 --------------------
// 키워드를 빨간색으로 강조해서 출력하는 함수
static void print_line_with_highlight(const char *line, const char *keyword) {
    const char *pos = line;
    const char *found;
    int keyword_len = strlen(keyword);
    
    while ((found = strstr(pos, keyword)) != NULL) {
        // 키워드 이전 부분 출력
        fwrite(pos, 1, found - pos, stdout);
        // 키워드를 빨간색으로 출력
        printf("%s%s%s", COLOR_RED, keyword, COLOR_RESET);
        pos = found + keyword_len;
    }
    // 나머지 부분 출력
    printf("%s", pos);
}

// -------------------- 검색 로직 --------------------
static int is_target_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;

    return (strcmp(ext, ".c") == 0 ||
            strcmp(ext, ".txt") == 0 ||
            strcmp(ext, ".h") == 0 ||
            strcmp(ext, ".py") == 0 ||
            strcmp(ext, ".md") == 0);
}

static void search_in_file(const char *filepath, const char *keyword, int thread_id) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return;

    struct stat st;
    if (stat(filepath, &st) != 0) {
        fclose(fp);
        return;
    }

    char line[1024];
    int line_num = 0;
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        if (strstr(line, keyword)) {
            if (!found) {
                pthread_mutex_lock(&print_lock);

                printf("\n[Thread %d] 매칭: %s\n", thread_id, filepath);
                printf("  크기: %ld bytes\n", (long)st.st_size);

                char time_buf[64];
                struct tm *tm_info = localtime(&st.st_mtime);
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                printf("  수정: %s\n", time_buf);

                pthread_mutex_unlock(&print_lock);

                found = 1;

                pthread_mutex_lock(&stat_lock);
                total_matches++;
                pthread_mutex_unlock(&stat_lock);
            }

            pthread_mutex_lock(&print_lock);
            printf("  %4d: ", line_num+1);
            print_line_with_highlight(line, keyword);
            pthread_mutex_unlock(&print_lock);
        }
    }

    fclose(fp);
}

// -------------------- 디렉터리 스캔 (Producer) --------------------
static void scan_directory(const char *path, TaskQueue *q) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "경고: 디렉터리를 열 수 없습니다: %s\n", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||  // 현재 디렉토리
            strcmp(entry->d_name, "..") == 0) { // 부모 디렉토리
            continue;
        }

        char fullpath[MAX_PATH];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        // symlink 루프 방지 목적이면 lstat 고려 가능.
        if (stat(fullpath, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_directory(fullpath, q);
        } else if (S_ISREG(st.st_mode)) {
            if (is_target_extension(entry->d_name)) {
                // 스캔 카운트 증가 (대상 파일 기준)
                pthread_mutex_lock(&stat_lock);
                scanned_files++;
                pthread_mutex_unlock(&stat_lock);

                // 작업 큐에 추가
                queue_push(q, fullpath);
            }
        }
    }

    closedir(dir);
}

// -------------------- Worker --------------------
typedef struct {
    TaskQueue *q;
    const char *keyword;
    int thread_id;
} WorkerArg;

static void* worker_thread(void *arg) {
    WorkerArg *wa = (WorkerArg*)arg;
    TaskQueue *q = wa->q;

    while (1) {
        pthread_mutex_lock(&q->lock);   // Mutex로 동기화

        while (q->count == 0 && !q->scan_done) {
            pthread_cond_wait(&q->cond, &q->lock);      // Condition Variable : 작업 없으면 스레드를 대기 상태로 전환
        }

        // 작업도 없고, 스캔도 끝났으면 종료
        if (q->count == 0 && q->scan_done) {
            pthread_mutex_unlock(&q->lock);
            break;
        }

        // 작업 하나 pop
        char *filepath = queue_pop(q);
        pthread_mutex_unlock(&q->lock);

        if (filepath) {
            search_in_file(filepath, wa->keyword, wa->thread_id);       // 검색
            free(filepath);
        }
    }

    return NULL;
}

// -------------------- main --------------------
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("사용법: %s [경로] [키워드]\n", argv[0]);
        printf("예시: %s /home/pi/project \"TODO\"\n", argv[0]);
        return 1;
    }

    const char *search_path = argv[1];
    const char *keyword = argv[2];

    struct stat st;
    if (stat(search_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "에러: '%s'는 유효한 디렉터리가 아닙니다.\n", search_path);
        return 1;
    }

    printf("=== 멀티스레드 파일 검색기 ===\n");
    printf("검색 경로: %s\n", search_path);
    printf("검색 키워드: \"%s\"\n", keyword);
    printf("스레드 개수: %d\n\n", MAX_THREADS);

    TaskQueue q;
    queue_init(&q);

    // 시간 측정 시작
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Worker 생성
    pthread_t threads[MAX_THREADS];
    WorkerArg args[MAX_THREADS];

    for (int i = 0; i < MAX_THREADS; i++) {
        args[i].q = &q;
        args[i].keyword = keyword;
        args[i].thread_id = i + 1;
        if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    // Producer: 디렉터리 스캔하면서 작업 push
    printf("📁 파일 탐색 + 검색 중...\n");
    scan_directory(search_path, &q);

    // 스캔 완료 신호
    pthread_mutex_lock(&q.lock);
    q.scan_done = 1;
    pthread_cond_broadcast(&q.cond);
    pthread_mutex_unlock(&q.lock);

    // Worker 종료 대기
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // 시간 측정 종료
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1000000000.0;

    printf("\n");
    printf("========================================\n");
    printf("검색 완료!\n");
    printf("총 %lld개 파일 스캔, %lld개 파일에서 매칭\n", scanned_files, total_matches);
    printf("소요 시간: %.3f초\n", elapsed);
    printf("========================================\n");

    queue_destroy(&q);
    pthread_mutex_destroy(&print_lock);
    pthread_mutex_destroy(&stat_lock);

    return 0;
}
