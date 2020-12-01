#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <time.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "netlib.h"
#include "debug.h"

#define MAX_EVENTS 30000

struct timespec timer_start(void) {
	struct timespec s; 
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &s) < 0) {
		printf("clock_gettime failed: %d\n", errno);
		exit(1);
	}
	return s;
}

uint64_t timer_end(struct timespec start) {
	struct timespec end;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end) < 0) {
		printf("clock_gettime failed: %d\n", errno);
		exit(1);
	}
	uint64_t base = (end.tv_sec - start.tv_sec) * 1000000000;
	return base + end.tv_nsec - start.tv_nsec;
}
	

int main(int argc, char** argv) {
	char *conf_file = NULL;
	int o, ret;
	int is_client = 0;
	int is_server = 0;
	int core_limit = 1;
	int num_iters = 0;

	while (-1 != (o = getopt(argc, argv, "f:hn:sc"))) {
		switch (o) {
			case 'f': 
				conf_file = optarg;
				break;
			case 'h':
				fprintf(stderr, "USAGE: %s -f <conf_file> -n <num_iters> (-s|-c) [-h]\n", argv[0]);
				break;
			case 'n':
				num_iters = mystrtol(optarg, 10);
				break;
			case 'c':
				is_client = 1;
				break;
			case 's':
				is_server = 1;
				break;
		}
	}

	struct mtcp_conf mcfg;
	mtcp_getconf(&mcfg);
	mcfg.num_cores = core_limit;
	mtcp_setconf(&mcfg);
	
	if (num_iters == 0) {
		TRACE_CONFIG("Need num_iters > 0\n");
		exit(EXIT_FAILURE);
	}
	
	if (conf_file == NULL) {
		TRACE_CONFIG("mTCP startup config file required\n");
		exit(EXIT_FAILURE);
	}

	if (!is_client && !is_server) {
		TRACE_CONFIG("Set either -s (for server) or -c (for client)\n");
		exit(EXIT_FAILURE);
	}
	printf("is_client: %d, is_server: %d\n", is_client, is_server);
	
	ret = mtcp_init(conf_file);
	if (ret) {
		TRACE_CONFIG("Failed to initialize mTCP\n");
		exit(EXIT_FAILURE);
	}

	TRACE_INFO("Application initialization finished %d %d.\n", is_client, is_server);

	mtcp_core_affinitize(0);
	mctx_t mctx = mtcp_create_context(0);
	if (!mctx) {
		printf("Failed to create mTCP context\n");
		return 1;
	}
	int ep = mtcp_epoll_create(mctx, MAX_EVENTS);
	if (ep < 0) {
		printf("Failed to create epoll descriptor");
		return 1;
	}
	struct mtcp_epoll_event *events = (struct mtcp_epoll_event*)calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		printf("Failed to create event struct\n");
		return 1;
	}

	if (is_server) {
		int listener = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
		if (listener < 0) {
			printf("Failed to create listening socket\n");
			return 1;
		}
		ret = mtcp_setsock_nonblock(mctx, listener);
		if (ret < 0) {
			printf("Failed to set nonblocking socket\n");
			return 1;
		}
		struct sockaddr_in saddr;
		saddr.sin_family = AF_INET;
		saddr.sin_addr.s_addr = INADDR_ANY;
		saddr.sin_port = htons(80);
		ret = mtcp_bind(mctx, listener, (struct sockaddr*)&saddr, sizeof(struct sockaddr_in));
		if (ret < 0) {
			printf("Failed to bind socket\n");
			return 1;
		}
		ret = mtcp_listen(mctx, listener, 4);
		if (ret < 0) {
			printf("Failed to listen on socket\n");
			return 1;
		}

		struct mtcp_epoll_event ev;
		ev.events = MTCP_EPOLLIN;
		ev.data.sockid = listener;
		mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

		int fd = -1;
		printf("Accepting...\n");
		while (fd == -1) {
			// printf("fak waiting for connect\n");
			int nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
			if (nevents < 0) {
				// printf("epoll_wait failed: %d\n", errno);
				return 1;
			}
			int do_accept = 0;
			for (int i = 0; i < nevents; i++) {
				if (events[i].data.sockid == listener) {
					do_accept = 1;
				}
			}
			if (do_accept) {
				ret = mtcp_accept(mctx, listener, NULL, NULL);
				if (ret < 0) {
					if (errno == EAGAIN) {
						// printf("fak EAGAIN\n");
						continue;
					}
					// printf("mtcp_accept failed: %d\n", errno);
					return 1;
				}
				struct mtcp_epoll_event ev;
				ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
				ev.data.sockid = ret;
				mtcp_setsock_nonblock(mctx, ret);
				mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, ret, &ev);
				fd = ret;
			}
		}
		printf("Accepted!\n");

		char buf[64];
		for (int i = 0; ; i++) {
			// printf("Round %d\n", i);
			int num_read = 0;
			int wait = 0;
			while (num_read < 64) {
				int do_read = 1;
				if (wait) {
					// printf("fak waiting for in\n");
					int nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
			    		if (nevents < 0) {
			    			// printf("epoll_wait failed: %d\n", errno);
			    			return 1;
			    		}
			    		do_read = 0;
			    		for (int j = 0; j < nevents; j++) {
			    			if (events[j].events & MTCP_EPOLLIN) {
			    				do_read = 1;
			    			}
			    		}
				}
				if (do_read) {
					int bytes_read = mtcp_read(mctx, fd, &buf[num_read], 64 - num_read);
					if (bytes_read < 0) {
						if (errno == EAGAIN || errno == ENOTCONN) {
							// printf("fak EAGAIN: %d\n", errno);
							wait = 1;
							continue;
						}
						// printf("mtcp_read failed: %d\n", errno);
						return 1;
					}
					num_read += bytes_read;
					// printf("read %d bytes\n", bytes_read);
				}
			}
			int num_written = 0;
			wait = 0;
			while (num_written < 64) {
				int do_write = 1;
				if (wait) {
					// printf("fak waiting for write\n");
					int nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
					if (nevents < 0) {
						// printf("epoll_wait failed: %d\n", errno);
						return 1;
					}
					do_write = 0;
					for (int j = 0; j < nevents; j++) {
						if (events[j].events & MTCP_EPOLLOUT) {
							do_write = 1;
						}
					}
				}
				if (do_write) {
					int bytes_written = mtcp_write(mctx, fd, &buf[num_written], 64 - num_written);
					if (bytes_written < 0) {
						if (errno == EAGAIN) {
							// printf("fak EAGAIN\n");
							wait = 1;
							continue;
						}
						// printf("mtcp_write failed: %d\n", errno);
						return 1;
					}
					num_written += bytes_written;
					// printf("wrote %d bytes\n", bytes_written);
				}
			}
		}

		// int fd = -1;
		// while (fd == -1) {
		// 	int fd = mtcp_accept(mctx, listener, NULL, NULL);
		// 	if (fd < 0) {
		// 		if (errno == EAGAIN) {
		// 			continue;
		// 		}
		// 		printf("Failed to accept from socket\n");
		// 		return 1;
		// 	}
		// }
		// printf("Accepted connection\n");

		// ret = mtcp_setsock_nonblock(mctx, listener);
		// if (ret < 0) {
		// 	printf("Failed to set nonblocking socket\n");
		// 	return 1;
		// }

		// char buf[64];
		// for (int i = 0; i < num_iters; i++) {
		// 	int num_read = 0;
		// 	while (num_read < 64) {
		// 		int bytes_read = mtcp_read(mctx, fd, &buf[num_read], 64 - num_read);
		// 		if (bytes_read < 0) {
		// 			if (errno == EAGAIN) {
		// 				continue;
		// 			}
		// 			printf("Failed to read from socket: %d\n", errno);
		// 			return 1;
		// 		}
		// 		num_read += bytes_read;
		// 	}
		// 	int num_written = 0;
		// 	while (num_written < 64) {
		// 		int bytes_written = mtcp_write(mctx, fd, &buf[num_written], 64 - num_written);
		// 		if (bytes_written < 0) {
		// 			if (errno == EAGAIN) {
		// 				continue;
		// 			}
		// 			printf("Failed to write to socket: %d\n", errno);
		// 			return 1;
		// 		}
		// 		num_written += bytes_written;
		// 	}
		// }

		if (mtcp_close(mctx, fd) < 0) {
			printf("Failed to close socket\n");
		}
		if (mtcp_close(mctx, listener) < 0) {
			printf("Failed to close listening socket\n");
		}
			
	} else {

		int socket = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
		if (socket < 0) {
			printf("Failed to create socket\n");
			return 1;
		}
		ret = mtcp_setsock_nonblock(mctx, socket);
		if (ret < 0) {
			printf("Failed to set nonblocking socket\n");
			return 1;
		}
		struct sockaddr_in saddr;
		saddr.sin_family = AF_INET;
		saddr.sin_addr.s_addr = inet_addr("198.19.200.4");
		saddr.sin_port = htons(80);
		printf("Connecting...\n");	

		ret = mtcp_connect(mctx, socket, (struct sockaddr*)&saddr, sizeof(struct sockaddr_in));
		if (ret < 0 && errno != EINPROGRESS) {
			printf("Failed to connect\n");
			return 1;
		}

		struct mtcp_epoll_event ev;
		ev.events = MTCP_EPOLLOUT | MTCP_EPOLLIN;
		ev.data.sockid = socket;
		mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, socket, &ev);

		char buf[64];
		for (int i = 0; i < 64; i++) {
			buf[i] = 'a';
		}

		uint64_t *timing_buf = calloc(num_iters, sizeof(uint64_t));
		if (timing_buf == NULL) {
			printf("Failed to allocate timing buffer\n");
			return 1;
		}
		int timing_ptr = 0;

		for (int i = 0; i < num_iters; i++) {
			// printf("Round %d\n", i);
			struct timespec start = timer_start();	
                        int num_written = 0;
                        int wait = 0;
                        while (num_written < 64) {
                                int do_write = 1;
                                if (wait) {
                                        // printf("fak waiting for write\n");
                                        int nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
                                        if (nevents < 0) {
                                                // printf("epoll_wait failed: %d\n", errno);
                                                return 1;
                                        }
                                        do_write = 0;
                                        for (int j = 0; j < nevents; j++) {
                                                if (events[j].events & MTCP_EPOLLOUT) {
                                                        do_write = 1;
                                                }
                                        }
                                }
                                if (do_write) {
                                        int bytes_written = mtcp_write(mctx, socket, &buf[num_written], 64 - num_written);
                                        if (bytes_written < 0) {
                                                if (errno == EAGAIN || errno == ENOTCONN) {
                                                        // printf("fak EAGAIN: %d\n", errno);
                                                        wait = 1;
                                                        continue;
                                                }
                                                // printf("mtcp_write failed: %d\n", errno);
                                                return 1;
                                        }
					// printf("wrote %d bytes\n", bytes_written);
                                        num_written += bytes_written;
                                }
                        }
                        int num_read = 0;
                        wait = 0;
                        while (num_read < 64) {
                                int do_read = 1;
                                if (wait) {
                                        // printf("fak waiting for in\n");
                                        int nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
                                        if (nevents < 0) {
                                                // printf("epoll_wait failed: %d\n", errno);
                                                return 1;
                                        }
                                        do_read = 0;
                                        for (int j = 0; j < nevents; j++) {
                                                if (events[j].events & MTCP_EPOLLIN) {
                                                        do_read = 1;
                                                }
                                        }
                                }
                                if (do_read) {
                                        int bytes_read = mtcp_read(mctx, socket, &buf[num_read], 64 - num_read);
                                        if (bytes_read < 0) {
                                                if (errno == EAGAIN) {
                                                        // printf("fak EAGAIN\n");
                                                        wait = 1;
                                                        continue;
                                                }
                                                // printf("mtcp_read failed: %d\n", errno);
                                                return 1;
                                        }
                                        num_read += bytes_read;
					// printf("read %d bytes\n", bytes_read);
                                }
                        }                                
			for (int j = 0; j < 64; j++) {
				if (buf[j] != 'a') {
					printf("Invalid buffer returned: %d\n", buf[j]);
					return 1;
				}
			}

			uint64_t duration_ns = timer_end(start);
			timing_buf[timing_ptr] = duration_ns;
			timing_ptr++;
		}

		int timing_fd = open("timing.log", O_TRUNC | O_CREAT | O_RDWR, S_IRWXU);
		if (timing_fd < 0) {
			printf("Failed to open timing.log: %d\n", errno);
			return 1;
		}
		for (int i = 0; i < timing_ptr; i++) {
			dprintf(timing_fd, "%ld\n", timing_buf[i]);	
		}
		close(timing_fd);
		// char buf[64];
		// for (int i = 0; i < 64; i++) {
		// 	buf[i] = 'a';
		// }
		// for (int i = 0; i < num_iters; i++) {
		// 	int num_written = 0;
		// 	while (num_written < 64) {
		// 		int bytes_written = mtcp_write(mctx, socket, &buf[num_written], 64 - num_written);
		// 		if (bytes_written < 0) {
		// 			if (errno == EAGAIN) {
		// 				continue;
		// 			}
		// 			printf("Failed to write to socket: %d\n", errno);
		// 			return 1;
		// 		}
		// 		num_written += bytes_written;
		// 	}
		// 	int num_read = 0;
		// 	while (num_read < 64) {
		// 		int bytes_read = mtcp_read(mctx, socket, &buf[num_read], 64 - num_read);
		// 		if (bytes_read < 0) {
		// 			if (errno == EAGAIN) {
		// 				continue;
		// 			}
		// 			printf("Failed to read from socket: %d\n", errno);
		// 			return 1;
		// 		}
		// 		num_read += bytes_read;
		// 	}
		// 	for (int i = 0; i < 64; i++) {
		// 		if (buf[i] != 'a') {
		// 			printf("Echoed buffer didn't have fill character: %d\n", buf[i]);
		// 			return 1;
		// 		}
		// 	}
		// }

	}

	return 0;
}
