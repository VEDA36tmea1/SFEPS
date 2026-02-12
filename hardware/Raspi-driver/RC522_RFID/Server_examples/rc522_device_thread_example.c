/*
 * RC522 서버 연동: 디바이스 파일 직접 읽기 스레드 예제
 *
 * 라즈베리 파이가 서버일 때 권장 방식.
 * - 서버 프로세스 안에서 별도 스레드가 /dev/rc522 를 열고
 *   ioctl(RC522_READ_CARD) → ioctl(RC522_READ_TEXT_SECTOR) 로
 *   태깅 이벤트(device_id, id, text, timestamp)를 읽어 콜백으로 전달.
 * - 내부에서는 구조체만 사용. JSON은 외부 API로 보낼 때만 직렬화하면 됨.
 *
 * 빌드: gcc -o rc522_device_thread_example rc522_device_thread_example.c -I../Kernel_Driver -lpthread
 * 실행: sudo ./rc522_device_thread_example
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rc522_ioctl.h"

#define DEV_RC522       "/dev/rc522"
#define DEVICE_ID       1
#define DEFAULT_TRAILER 11

typedef struct {
	int device_id;
	uint32_t id;
	char text[48];
	time_t timestamp;
} rc522_tag_event_t;

static volatile int running = 1;
static int g_trailer = DEFAULT_TRAILER;

/*
 * 태깅 이벤트 수신 시 서버에서 할 일.
 * 예: DB 저장, 큐에 넣기, HTTP 전송, JSON 직렬화 후 전송 등.
 */
static void on_tag_event(const rc522_tag_event_t *ev)
{
	/* 예: 서버 로직 (로그만 찍는 예시) */
	printf("[태깅] device_id=%d id=%08X text=\"%s\" timestamp=%ld\n",
	       ev->device_id, (unsigned)ev->id, ev->text, (long)ev->timestamp);
	/* JSON으로 보낼 때만 여기서 직렬화:
	 * printf("{\"device_id\":%d,\"id\":\"%08X\",\"text\":\"...\",\"timestamp\":%ld}\n", ...);
	 */
}

static void *rc522_reader_thread(void *arg)
{
	int fd;
	uint32_t uid;
	struct rc522_read_text text_data;
	rc522_tag_event_t ev;

	(void)arg;

	fd = open(DEV_RC522, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_RC522);
		return NULL;
	}

	while (running) {
		if (ioctl(fd, RC522_READ_CARD, &uid) < 0) {
			if (errno == EINTR)
				break;
			usleep(100000);
			continue;
		}

		ev.device_id = DEVICE_ID;
		ev.id = uid;
		ev.timestamp = time(NULL);
		ev.text[0] = '\0';

		memset(&text_data, 0, sizeof(text_data));
		text_data.trailer_block = g_trailer;
		if (ioctl(fd, RC522_READ_TEXT_SECTOR, &text_data) == 0) {
			strncpy(ev.text, text_data.text, sizeof(ev.text) - 1);
			ev.text[sizeof(ev.text) - 1] = '\0';
			ev.id = text_data.uid;
		}

		on_tag_event(&ev);
		usleep(500000); /* 연속 중복 방지 */
	}

	close(fd);
	return NULL;
}

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	pthread_t th;

	if (argc >= 3 && strcmp(argv[1], "--trailer") == 0) {
		g_trailer = atoi(argv[2]);
		if (g_trailer < 0 || (g_trailer + 1) % 4 != 0) {
			fprintf(stderr, "trailer는 11, 15, 19, ... (4의 배수-1) 이어야 함\n");
			return 1;
		}
	}

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	printf("디바이스 직접 읽기 스레드 시작. device_id=%d, trailer=%d. Ctrl+C 종료.\n", DEVICE_ID, g_trailer);

	if (pthread_create(&th, NULL, rc522_reader_thread, NULL) != 0) {
		perror("pthread_create");
		return 1;
	}

	pthread_join(th, NULL);
	printf("종료.\n");
	return 0;
}
