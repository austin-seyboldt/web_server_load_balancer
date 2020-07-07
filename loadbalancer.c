#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <err.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <semaphore.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include "queue.h"

#define SERVER_ADDRESS "127.0.0.1"
#define SERVER_ADDRESS_LENGTH 10
#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
#define PORT_MIN 0
#define DEFAULT_REQUESTS_BEFORE_HEALTHCHECK 5
#define DEFAULT_WORKER_THREAD_COUNT 4
#define BACK_LOG 100
#define RESPONSE_500 "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"
#define CONNECTION_TIMEOUT_MILLSEC 2000
#define FORWARD_BUFFER_SIZE 8192
#define HEALTHCHECK_TIME_SEC 0
#define HEALTHCHECK_TIME_NANOSEC 800000000
#define MAX_NANOSEC 1000000000
#define HEALTHCHECK_TIMEOUT_MILLSEC 1000
#define HEALTH_RESPONSE_BUFFER_SIZE 256
#define HEALTH_REQUEST "GET /healthcheck HTTP/1.1\r\nContent-Length: 0\r\n\r\n"
#define SMALL_BUFF 64

// Used for debugging
//#define DEBUG
#ifdef DEBUG
#  define D(x) x
#else 
#  define D(x) 
#endif


typedef struct server_info 
{
	uint16_t port;
	size_t total_requests;
	size_t num_failures;
	char address [SERVER_ADDRESS_LENGTH];
	bool is_down;

} server_info;

typedef struct program_arguments
{
	uint16_t listen_port;
	uint16_t* server_ports;
	size_t server_count;
	size_t requests_before_healthcheck;
	size_t worker_thread_count;

} program_arguments;

typedef struct worker_args
{
	pthread_cond_t* wake_healthcheck;
	pthread_mutex_t* dispatch_lock;
	pthread_cond_t* client_ready;
	pthread_mutex_t* server_array_lock;
	pthread_mutex_t* health_request_update_lock;

	size_t requests_before_healthcheck;
	size_t* requests_since_healthcheck;
	server_info* servers;
	size_t server_count;
	JobQueue* job_pool;
	size_t thread_id;

} worker_args;

typedef struct healthcheck_args
{
	sem_t* health_init;
	pthread_cond_t* wake_healthcheck;
	pthread_mutex_t* server_array_lock;
	pthread_mutex_t* health_request_update_lock;

	size_t requests_before_healthcheck;
	size_t* requests_since_healthcheck;
	server_info* servers;
	size_t server_count;

} healthcheck_args;

typedef struct health_worker_args
{
	server_info* serv_info; 				// info about this worker's server
	pthread_mutex_t* worker_wake_lock;		// used to check worker_wake_count
	pthread_cond_t* worker_wake; 			// used to wake up the worker
	pthread_barrier_t* worker_barrier;		// used to sync all workers/main health thread
	size_t* worker_wake_count;				// count of awake threads
	size_t thread_id;
} health_worker_args;


int server_init(struct sockaddr_in* server_addr, uint16_t port);
bool parse_args(program_arguments*, int argc, char** argv);
uint16_t get_port(char* port);

void* healthcheck_thread(void*);
void* worker_thread(void*);
void* health_worker_thread(void* health_args);

server_info* get_optimal_server(server_info* const, size_t);
bool server_compare(const server_info* current_server, server_info* const best_server);
int connect_to_server(server_info* server);
void send_500_response(int client_sock);
int bridge_connection(int client_sock, int server_sock);
int forward_data(int in_fd, int out_fd);
void perform_health_check(server_info* server);
bool get_health_check(int server_fd, uint8_t* buf);
bool good_response(uint8_t* buf);


int main(int argc, char** argv)
{
	// parse arguments and options
	program_arguments args;
	if (!parse_args(&args, argc, argv))
	{
		return EXIT_FAILURE;
	}

	// intialize server socket
	struct sockaddr_in server_addr;
	int server_sock = server_init(&server_addr, args.listen_port);
	if (server_sock == -1)
	{
		return EXIT_FAILURE;
	}
	D(printf("Initialized server on port %i\n", args.listen_port));

	// intialize mutual exclusion devices*********************************************
	sem_t health_init;
	sem_init(&health_init, 0, 0);
	pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t client_ready = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t server_array_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t health_request_update_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t wake_healthcheck = PTHREAD_COND_INITIALIZER;
	// *******************************************************************************


	// intialize shared structs and variables*****************************************
	JobQueue* job_pool = JobQueue_create();
	server_info* servers = (server_info*) malloc(args.server_count * sizeof(server_info));
	for (size_t i = 0; i < args.server_count; i++)
	{
		servers[i].port = args.server_ports[i];
		servers[i].total_requests = 0;
		servers[i].num_failures = 0;
		strncpy(servers[i].address, SERVER_ADDRESS, SERVER_ADDRESS_LENGTH);
		servers[i].is_down = true;
	}
	size_t requests_since_healthcheck = 0;
	// *******************************************************************************


	// initialize healthcheck thread****************************************************
	healthcheck_args health_args;
	health_args.health_init = &health_init;
	health_args.wake_healthcheck = &wake_healthcheck;
	health_args.server_array_lock = &server_array_lock;
	health_args.health_request_update_lock = &health_request_update_lock;
	health_args.requests_before_healthcheck = args.requests_before_healthcheck;
	health_args.requests_since_healthcheck = &requests_since_healthcheck;
	health_args.servers = servers;
	health_args.server_count = args.server_count;

	pthread_t* health_thread = (pthread_t*) malloc(sizeof(pthread_t));
	pthread_create(health_thread, NULL, healthcheck_thread, (void*)&health_args);
	// **********************************************************************************



	// initialize worker threads*********************************************************
	worker_args* worker_thread_args = (worker_args*) malloc(args.worker_thread_count 
															* sizeof(worker_args));
	pthread_t* worker_threads = (pthread_t*) malloc(args.worker_thread_count * sizeof(pthread_t));
	for (size_t i = 0; i < args.worker_thread_count; i++)
	{
		worker_thread_args[i].wake_healthcheck = &wake_healthcheck;
		worker_thread_args[i].dispatch_lock = &dispatch_lock;
		worker_thread_args[i].client_ready = &client_ready;
		worker_thread_args[i].server_array_lock = &server_array_lock;
		worker_thread_args[i].health_request_update_lock = &health_request_update_lock;
		worker_thread_args[i].requests_before_healthcheck = args.requests_before_healthcheck;
		worker_thread_args[i].requests_since_healthcheck = &requests_since_healthcheck;
		worker_thread_args[i].servers = servers;
		worker_thread_args[i].server_count = args.server_count;
		worker_thread_args[i].job_pool = job_pool;
		worker_thread_args[i].thread_id = i;

		pthread_create(&worker_threads[i], NULL, worker_thread, (void*)&worker_thread_args[i]);
	}
	// ***********************************************************************************


	// wait until first healthcheck has been performed
	sem_wait(&health_init);
	D(printf("main() finished waiting for healthcheck to initialize\n"));

	// dispatcher
	while (true)
	{
		// Prepare client socket and accept connection
		struct sockaddr client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_sock  = accept(server_sock, &client_addr, &client_len);

		if (client_sock == -1)
		{
			perror("accept()");
			continue;
		}
		D(printf("Connection accepted.\n"));

		// Add the connection to the job pool
		pthread_mutex_lock(&dispatch_lock);
		job_pool->enqueue(job_pool, client_sock);
		pthread_cond_signal(&client_ready);
		pthread_mutex_unlock(&dispatch_lock);
	}

	return EXIT_SUCCESS;
}





// *****************************************************************************************
// Worker Thread Functions *****************************************************************
void* worker_thread(void* work_args)
{
	worker_args* args = (worker_args*) work_args;
	JobQueue* job_pool = args->job_pool;
	pthread_mutex_t* dispatch_lock = args->dispatch_lock;
	pthread_cond_t* client_ready = args->client_ready;
	pthread_mutex_t* server_lock = args->server_array_lock;
	D(printf("Worker thread [%li] initialized\n", args->thread_id));

	while (true)
	{
		// grab client connection *******************************
		pthread_mutex_lock(dispatch_lock);
		while (job_pool->empty(job_pool))
		{
			pthread_cond_wait(client_ready, dispatch_lock);
		}
		int client_sock = job_pool->dequeue(job_pool);
		pthread_mutex_unlock(dispatch_lock);
		// ******************************************************

		// get optimal server info *****************************************
		pthread_mutex_lock(server_lock);
		server_info* server = get_optimal_server(args->servers, args->server_count);
		if (server != NULL)
		{
			(server->total_requests)++;
		}
		pthread_mutex_unlock(server_lock);
		// *****************************************************************

		// update count of requests since last healthcheck
		pthread_mutex_lock(args->health_request_update_lock);
		(*args->requests_since_healthcheck)++;
		if ((*args->requests_since_healthcheck) >= (args->requests_before_healthcheck))
		{
			pthread_cond_signal(args->wake_healthcheck);
			D(printf("Signalling health thread to wake up...\n"));
		}		
		pthread_mutex_unlock(args->health_request_update_lock);

		// connect to optimal server (get socket) **************************
		int server_sock;
		if ((server_sock = connect_to_server(server)) == -1)
		{
			if (server != NULL)
			{
				pthread_mutex_lock(server_lock);
				server->is_down = true;
				pthread_mutex_unlock(server_lock);
			}
			send_500_response(client_sock);
			if (close(client_sock) == -1)
			{
				perror("worker_thread():close()");
			}
			continue;
		}
		// ****************************************************************

		D(printf("Worker thread [%lu] connecting to server on port [%hu]\n", args->thread_id, server->port));
		// forward the connection
		if (bridge_connection(client_sock, server_sock) == -1)
		{
			pthread_mutex_lock(server_lock);
			server->is_down = true;
			pthread_mutex_unlock(server_lock);
		}

		if (close(client_sock) == -1)
		{
			perror("worker_thread():close()");
		}
		if (close(server_sock) == -1)
		{
			perror("worker_thread():close()");
		}

	}

}

server_info* get_optimal_server(server_info* const servers, size_t server_count)
{
	server_info* best_server = NULL;

	for (size_t cur = 0; cur < server_count; cur++)
	{
		if (server_compare(&servers[cur], best_server))
		{
			best_server = &servers[cur];
		}
	}

	return best_server;
}

bool server_compare(const server_info* current_server, server_info* const best_server)
{
	if (best_server == NULL && !current_server->is_down)
	{
		return true;
	}
	if (current_server->is_down)
	{
		return false;
	}
	if (current_server->total_requests < best_server->total_requests)
	{
		return true;
	}
	if (current_server->total_requests > best_server->total_requests)
	{
		return false;
	}
	float cur_ratio = current_server->num_failures / current_server->total_requests;
	float best_ratio = best_server->num_failures / best_server->total_requests;
	if (cur_ratio < best_ratio)
	{
		return true;
	}
	return false;
}

int connect_to_server(server_info* server)
{
	if (server == NULL)
	{
		return -1;
	}

	int sock;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1)
	{
		return -1;
	}

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(server->port);
	inet_pton(AF_INET, server->address, &(servaddr.sin_addr));

	while (connect(sock, (struct sockaddr*) &servaddr, sizeof(servaddr)) == -1)
	{
		if (errno == EINTR)
		{
			continue;
		}
		if (close(sock) == -1)
		{
			D(perror("connect_to_server():close()"));
		}
		D(perror("connect_to_server():connect()"));
		return -1;
	}


	return sock;
}

void send_500_response(int client_sock)
{
	D(printf("Entered send_500_response()\n"));
	if (client_sock == -1)
		return;

	while (send(client_sock, RESPONSE_500, strlen(RESPONSE_500), 0) == -1)
	{
		if (errno == EINTR)
		{
			continue;
		}
		perror("send_500_response():send()");
		return;
	}

}

int bridge_connection(int client_sock, int server_sock)
{
	D(printf("Entered bridge_connection(%i, %i)\n", client_sock, server_sock));
	struct pollfd fds[2];
	fds[0].fd = client_sock;
	fds[0].events = POLLIN;
	fds[1].fd = server_sock;
	fds[1].events = POLLIN;

	bool server_responded = false;
	while (true)
	{
		int ret;
		while ((ret = poll(&fds[0], 2, CONNECTION_TIMEOUT_MILLSEC)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			fprintf(stderr, "bridge_connection():poll(): %s\n", strerror(ret));
			return -1;
		}
		// timeout
		if (ret == 0)
		{
			if (!server_responded)
			{
				send_500_response(client_sock);
			}
			fprintf(stderr, "bridge_connecion(): poll() timed out\n");
			return -1;
		}
		D(printf("Data ready to forward -- Client: %i\tServer: %i\n", client_sock, server_sock));
		// there's data to read from client
		if ((fds[0].revents & POLLIN) == POLLIN)
		{
			if (forward_data(client_sock, server_sock) == -1)
			{
				return 0;
			}
		}
		// there's data to read from server
		if ((fds[1].revents & POLLIN) == POLLIN)
		{
			if (forward_data(server_sock, client_sock) == -1)
			{
				return 0;
			}
			server_responded = true;
		}
	}
}

// Return 0 upon good execution, Return -1 if one end disconnects
int forward_data(int in_fd, int out_fd)
{
	D(printf("Entered forward_data(%i, %i)\n", in_fd, out_fd));
	ssize_t bytes_read = 0;
	
	uint8_t buf[FORWARD_BUFFER_SIZE];
	while ((bytes_read = recv(in_fd, &buf, FORWARD_BUFFER_SIZE, 0)) == -1)
	{
		if (errno == EINTR)
		{
			continue;
		}
		perror("forward_data():recv()");
		D(printf("forward_data():recv(%i)\n", in_fd));
		return -1;
	}
	D(printf("bytes read: %li\n", bytes_read));

	// Connection has been closed
	if (bytes_read == 0)
	{
		D(printf("Connection closed on read(%i)\n", in_fd));
		return -1;
	}

	ssize_t bytes_sent = 0;
	uint8_t* buf_offset = &buf[0];
	while (bytes_sent < bytes_read)
	{
		ssize_t bytes_sent_this_call;
		if ((bytes_sent_this_call = send(out_fd, buf_offset, bytes_read - bytes_sent, 0)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("forward_data():send()");
			return -1;
		}
		D(printf("bytes sent: %li\n", bytes_sent_this_call));
		bytes_sent += bytes_sent_this_call;
		buf_offset = (uint8_t*)(buf_offset + bytes_sent_this_call);
	}

	return 0;
}

// End worker thread functions *************************************************************
// *****************************************************************************************





// *****************************************************************************************
// Healthcheck Thread functions ************************************************************
void* healthcheck_thread(void* health_args)
{
	healthcheck_args* args = (healthcheck_args*) health_args;
	size_t server_count = args->server_count;
	server_info* servers = args->servers;
	pthread_mutex_t* server_lock = args->server_array_lock;
	pthread_mutex_t* health_request_update_lock = args->health_request_update_lock;
	pthread_cond_t* wake_healthcheck = args->wake_healthcheck;

	// Initialize mutual exclusion devices and shared data ******************
	server_info* temp_servers = (server_info*) malloc(server_count * sizeof(server_info));
	for (size_t i = 0; i < server_count; i++)
	{
		temp_servers[i] = servers[i];
	}
	size_t worker_wake_count = server_count;

	pthread_mutex_t worker_wake_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t worker_wake = PTHREAD_COND_INITIALIZER;
	pthread_barrier_t worker_barrier;
	pthread_barrier_init(&worker_barrier, NULL, server_count + 1);
	
	// **********************************************************************



	// Initialize workers ***************************************************
	health_worker_args* work_args = (health_worker_args*) malloc(server_count * 
															sizeof(health_worker_args));
	for (size_t i = 0; i < server_count; i++)
	{
		work_args[i].serv_info = &temp_servers[i];
		work_args[i].worker_wake_lock = &worker_wake_lock;
		work_args[i].worker_wake = &worker_wake;
		work_args[i].worker_barrier = &worker_barrier;
		work_args[i].worker_wake_count = &worker_wake_count;
		work_args[i].thread_id = i;
	}
	pthread_t* workers = (pthread_t*) malloc(server_count * sizeof(pthread_t));
	for (size_t i = 0; i < server_count; i++)
	{
		pthread_create(&workers[i], NULL, health_worker_thread, (void*)&work_args[i]);
	}

	// **********************************************************************

	pthread_barrier_wait(&worker_barrier);
	D(printf("Healthcheck thread initialized\n"));
	sem_post(args->health_init);

	while (true)
	{
		// Calculate wait time **********************************************
		struct timespec tm;
		clock_gettime(CLOCK_REALTIME, &tm);
		if ((HEALTHCHECK_TIME_NANOSEC + tm.tv_nsec) >= MAX_NANOSEC)
		{
			tm.tv_sec += 1;
		}
		tm.tv_sec += HEALTHCHECK_TIME_SEC;
		tm.tv_nsec = (HEALTHCHECK_TIME_NANOSEC + tm.tv_nsec) % MAX_NANOSEC;
		// ******************************************************************

		pthread_mutex_lock(&worker_wake_lock);
		while (*args->requests_since_healthcheck < args->requests_before_healthcheck)
		{
			int err;
			if ((err = pthread_cond_timedwait(wake_healthcheck, &worker_wake_lock, &tm)))
			{
				if (err == ETIMEDOUT)
				{
					break;
				}
				else
				{
					fprintf(stderr, "pthread_cond_timedwait(): %s\n", strerror(err));
					break;
				}
			}
		}
		worker_wake_count = server_count;
		pthread_cond_broadcast(&worker_wake);
		pthread_mutex_unlock(&worker_wake_lock);

		pthread_barrier_wait(&worker_barrier);

		pthread_mutex_lock(server_lock);
		for (size_t i = 0; i < server_count; i++)
		{
			servers[i] = temp_servers[i];
		}

		pthread_mutex_lock(health_request_update_lock);
		(*args->requests_since_healthcheck) = 0;
		pthread_mutex_unlock(health_request_update_lock);

		pthread_mutex_unlock(server_lock);
	}

}

void* health_worker_thread(void* health_args)
{
	health_worker_args* args = (health_worker_args*) health_args;
	server_info* server = args->serv_info;
	pthread_mutex_t* worker_wake_lock = args->worker_wake_lock;
	pthread_cond_t* worker_wake = args->worker_wake;
	pthread_barrier_t* worker_barrier = args->worker_barrier;
	size_t* worker_wake_count = args->worker_wake_count;

	D(printf("Health worker thread [%lu] initialized\n", args->thread_id));
	while (true)
	{
		pthread_mutex_lock(worker_wake_lock);
		while (*worker_wake_count == 0)
		{
			pthread_cond_wait(worker_wake, worker_wake_lock);
		}
		(*worker_wake_count)--;
		pthread_mutex_unlock(worker_wake_lock);

		D(printf("Health thread [%lu] about to perform health check...\n", args->thread_id));
		perform_health_check(server);
		if (server->is_down)
		{
			D(printf("\n-------------------------\nServer [%i] is down\n-------------------------\n\n", server->port));
		}

		pthread_barrier_wait(worker_barrier);
	}
}

void perform_health_check(server_info* server)
{
	int server_fd;
	if ((server_fd = connect_to_server(server)) == -1)
	{
		server->is_down = true;
		return;
	}
	
	// get the healthcheck
	uint8_t buf [HEALTH_RESPONSE_BUFFER_SIZE + 1];
	memset(&buf, 0, (HEALTH_RESPONSE_BUFFER_SIZE + 1) *sizeof(uint8_t));
	if (!get_health_check(server_fd, buf))
	{
		if (close(server_fd) == -1)
		{
			perror("perform_health_check():close()");
		}
		server->is_down = true;
		return;
	}
	if (close(server_fd) == -1)
	{
		perror("perform_health_check():close()");
	}
	
	// parse response / update server info
	if (!good_response(buf))
	{
		server->is_down = true;
		return;
	}
	
	char* msg_start = (strstr((char*)&buf[0], "\r\n\r\n") + 4);
	ssize_t total_requests = 0;
	ssize_t num_failures = 0;
	buf[HEALTH_RESPONSE_BUFFER_SIZE] = '\0';
	int ret = 0;
	if ((ret = sscanf(msg_start, "%li %li", &num_failures, &total_requests)) < 2)
	{
		perror("perform_health_check():sscanf()");
		D(printf("sscanf of request failed: %i\n", ret));
		D(printf("Bad String: %s\n", (char*)buf));
		server->is_down = true;
		return;
	}
	if ((total_requests < 0) || (num_failures < 0))
	{
		D(printf("request number bad\n"));
		server->is_down = true;
		return;
	}

	server->total_requests = (size_t)total_requests;
	server->num_failures = (size_t)num_failures;
	server->is_down = false;
	D(printf("Server [%i] total load: [%lu]\tfailures: [%lu]\n", server->port, server->total_requests, server->num_failures));
}

bool get_health_check(int server_fd, uint8_t* buf)
{
	// request healthcheck
	ssize_t bytes =0;
	ssize_t total_bytes_sent = 0;
	while ((size_t)total_bytes_sent < strlen(HEALTH_REQUEST))
	{
		while ((bytes = send(server_fd, HEALTH_REQUEST + (size_t)total_bytes_sent, strlen(HEALTH_REQUEST) - total_bytes_sent, 0)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("perform_health_check():send()");
			return false;
		}
		total_bytes_sent += bytes;
	}

	// get response
	struct pollfd fds[1];
	fds[0].fd = server_fd;
	fds[0].events = POLLIN;
	char* buffer = (char*)&buf[0];
	ssize_t total_bytes_recv = 0;
	while (true)
	{
		int ret;
		if ((ret = poll(&fds[0], 1, HEALTHCHECK_TIMEOUT_MILLSEC)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("get_health_check():poll()");
			return false;
		}
		// timeout
		if (ret == 0)
		{
			D(printf("Healthcheck timed out\n"));
			return false;
		}
		// read from the socket
		while ((bytes = recv(server_fd, buffer, HEALTH_RESPONSE_BUFFER_SIZE - total_bytes_recv, 0)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("get_health_check():recv()");
			return false;
		}
		//D(printf("Raw String: %s\n\n", (char*)buf));
		if (bytes == 0)
		{
			return true;
		}
		buffer += (size_t)bytes;
		total_bytes_recv += bytes;
	}
}

bool good_response(uint8_t* buf)
{
	// "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"
	char* next_line = strstr((char*)buf, "\r\n");
	if (next_line == NULL)
	{
		return false;
	}
	char protocol[SMALL_BUFF];
	memset(&protocol, 0, SMALL_BUFF * sizeof(char));
	uint16_t status_code;
	char status_msg[SMALL_BUFF];
	memset(&status_msg, 0, SMALL_BUFF * sizeof(char));
	if (sscanf((char*)buf, "%s %hu %s[^\r\n]", &protocol[0], &status_code, &status_msg[0]) < 3)
	{
		return false;
	}
	if (strcmp(protocol, "HTTP/1.1") != 0)
	{
		return false;
	}
	if (status_code != 200)
	{
		return false;
	}
	
	// get content-length header
	bool content_length_found = false;
	char* this_line;
	do
	{
		this_line = next_line + 2;
		next_line = strstr(this_line, "\r\n");
		char header_buf[SMALL_BUFF];
		char content_buf[SMALL_BUFF];
		memset(&header_buf, 0, SMALL_BUFF * sizeof(char));
		memset(&content_buf, 0, SMALL_BUFF * sizeof(char));
		if (sscanf(this_line, "%s %s", &header_buf[0], &content_buf[0]) < 2)
		{
			return false;
		}
		if (strcmp(header_buf, "Content-Length:") == 0)
		{
			if (content_length_found == true)
			{
				return false;
			}
			content_length_found = true;

			int64_t len = strtol(content_buf, NULL, 10);
			if (len <= 0)
			{
				return false;
			}
		}
	}while(strstr(next_line, "\r\n\r\n") != next_line);

	if (!content_length_found)
	{
		return false;
	}
	D(printf("Good health response\n"));
	return true;
}

// End healthcheck thread functions ********************************************************
// *****************************************************************************************





// *****************************************************************************************
// Main functions **************************************************************************
int server_init(struct sockaddr_in* server_addr, uint16_t port)
{
	// Initialize server address info
	server_addr->sin_family = AF_INET;
	server_addr->sin_port = htons(port);
	server_addr->sin_addr.s_addr = htonl(INADDR_ANY);

	// Create socket
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock == -1)
	{
		perror("server_init()");
		return -1;
	}

	int8_t ret = 0;

	// Configure socket, bind, set to listen
	socklen_t opt_val = 1;
	ret = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
	if (ret < 0)
	{
		perror("server_init():setsockopt()");
		return -1;
	}
	ret = bind(server_sock, (struct sockaddr*) server_addr, sizeof(struct sockaddr_in));
	if (ret < 0)
	{
		perror("server_init():bind()");
		return -1;
	}
	ret = listen(server_sock, BACK_LOG);

	if (ret < 0)
	{
		perror("server_init():listen()");
		return -1;
	}
	return server_sock;
}

bool parse_args(program_arguments* args, int argc, char** argv)
{
	int opt;
	bool R_found = false;
	bool N_found = false;
	while ((opt = getopt(argc, argv, "N:R:")) != -1)
	{
		if ((optarg == 0) || (*optarg == '-'))
		{
			opt = ':';
			optind--;
		}
		if (opt == 'R')
		{
			long int num = strtol(optarg, NULL, 10);
			if (num <= 0)
			{
				fprintf(stderr, "Invalid argument\n");
				return false;
			}
			R_found = true;
			args->requests_before_healthcheck = (size_t)num;
		}
		else if (opt == 'N')
		{
			long int num = strtol(optarg, NULL, 10);
			if (num <= 0)
			{
				fprintf(stderr, "Invalid argument\n");
				return false;
			}
			N_found = true;
			args->worker_thread_count = (size_t)num;
		}
		else if (opt == ':')
		{
			fprintf(stderr, "Option provided without corresponding argument\n");
			return false;
		}
		else if (opt == '?')
		{
			fprintf(stderr, "Option not recognized\n");
			return false;
		}
	}

	// get the port number
	size_t listen_port_index = optind;
	if (optind < argc)
	{
		if ((args->listen_port = get_port(argv[optind])) == 0)
		{
			fprintf(stderr, "Invalid port\n");
			return false;
		}
	}
	else
	{
		fprintf(stderr, "Missing port numbers\n");
		return false;
	}

	size_t num_servers = (size_t) (argc - (listen_port_index + 1));
	if (num_servers == 0)
	{
		fprintf(stderr, "Missing server port number\n");
		return false;
	}
	// get the server port numbers
	args->server_count = num_servers;
	uint16_t* server_ports = (uint16_t*)malloc(num_servers * sizeof(uint16_t));
	for (size_t arr_index = 0; arr_index < num_servers; arr_index++)
	{
		if ((server_ports[arr_index] = get_port(argv[arr_index + listen_port_index + 1])) == 0)
		{
			fprintf(stderr, "Invalid server port-number or argument\n");
			free(server_ports);
			return false;
		}
	}
	args->server_ports = server_ports;

	if (!R_found)
	{
		args->requests_before_healthcheck = DEFAULT_REQUESTS_BEFORE_HEALTHCHECK;
	}
	if (!N_found)
	{
		args->worker_thread_count = DEFAULT_WORKER_THREAD_COUNT;
	}
	
	return true;
}

uint16_t get_port(char* port)
{
	int64_t p = strtol(port, NULL, 10);
	if ((p <= PORT_MIN) || (p > UINT16_MAX))
	{
		return 0;
	}
	return (uint16_t)p;
}