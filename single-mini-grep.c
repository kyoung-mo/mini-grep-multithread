/**
 * 싱글스레드 파일 검색기 (single-mini-grep)
 *
 * 기능:
 * - 디렉터리 재귀 탐색
 * - 키워드 검색 및 매칭
 * - 키워드 빨간색 강조 (grep 스타일)
 *
 * 빌드:
 *   gcc single_mini_grep.c -o single_mini_grep
 *
 * 실행:
 *   ./single_mini_grep /path "TODO"
 */

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MAX_PATH
#define MAX_PATH 10000
#endif

// -------------------- ANSI 색상 코드 --------------------
#define COLOR_RED     "\033[1;31m"
#define COLOR_RESET   "\033[0m"

// -------------------- 전역 통계 --------------------
static long long scanned_files = 0;   // 스캔한 "대상 파일" 개수
static long long total_matches = 0;   // 매칭된 "파일" 개수(파일 단위)

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

static void search_in_file(const char *filepath, const char *keyword) {
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
                printf("\n매칭: %s\n", filepath);
                printf("  크기: %ld bytes\n", (long)st.st_size);

                char time_buf[64];
                struct tm *tm_info = localtime(&st.st_mtime);
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                printf("  수정: %s\n", time_buf);

                found = 1;
                total_matches++;
            }

            printf("  %4d: ", line_num);
            print_line_with_highlight(line, keyword);
        }
    }

    fclose(fp);
}

// -------------------- 디렉터리 스캔 --------------------
static void scan_directory(const char *path, const char *keyword) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "경고: 디렉터리를 열 수 없습니다: %s\n", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
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
            scan_directory(fullpath, keyword);
        } else if (S_ISREG(st.st_mode)) {
            if (is_target_extension(entry->d_name)) {
                // 스캔 카운트 증가 (대상 파일 기준)
                scanned_files++;
                
                // 파일 검색
                search_in_file(fullpath, keyword);
            }
        }
    }

    closedir(dir);
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

    printf("=== 싱글스레드 파일 검색기 ===\n");
    printf("검색 경로: %s\n", search_path);
    printf("검색 키워드: \"%s\"\n\n", keyword);

    // 시간 측정 시작
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // 디렉터리 스캔 및 검색
    printf("📁 파일 탐색 + 검색 중...\n");
    scan_directory(search_path, keyword);

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

    return 0;
}
