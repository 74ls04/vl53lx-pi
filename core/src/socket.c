#include <socket.h>

struct socket udp_socket;
pthread_mutex_t udp_socket_mutex;

int SocketClose(struct socket *sock)
{
	close(sock->fd);
	free(sock->ip);
	return 0;
}

int SocketSetup(struct socket *sock)
{
	int option = 1;
	int proto;
	memset(&sock->info, 0, sizeof(struct sockaddr_in));
	if (sock->type == SOCK_DGRAM)
	{
		proto = IPPROTO_UDP;
	}
	if (sock->type == SOCK_STREAM)
	{
		proto = IPPROTO_TCP;
	}
	if ((sock->fd = socket(AF_INET, sock->type, proto)) < 0)
	{
		close(sock->fd);
		return 0;
	}
	if (sock->blocking)
	{
		if (fcntl(sock->fd, F_SETFL, O_NONBLOCK, 1) == 0)
		{
			close(sock->fd);
			return 0;
		}
	}
	if (strcmp(sock->ip, "ANY") == 0)
	{
		sock->info.sin_family = AF_INET;
		sock->info.sin_addr.s_addr = htonl(INADDR_ANY);
		sock->info.sin_port = htons(sock->port);
		if(sock->type == SOCK_STREAM)
		{
			if(setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &option, sizeof(int)) < 0)
			{
				return 0;
			}
		}
		if (bind(sock->fd, (struct sockaddr *) &sock->info, sizeof(struct sockaddr_in)) < 0)
		{
			return 0;
		}
		if (sock->type == SOCK_STREAM)
		{
			listen(sock->fd, 1);
		}
	}
	else
	{
		sock->info.sin_family = AF_INET;
		sock->info.sin_addr.s_addr = inet_addr(sock->ip);
		sock->info.sin_port = htons(sock->port);
		if (sock->type == SOCK_STREAM)
		{
			if ((connect(sock->fd, (struct sockaddr *) &sock->info, sizeof(struct sockaddr_in))) < 0)
			{
				close(sock->fd);
				return 0;
			}
		}
	}
	return 1;
}

int SocketSelect(struct socket *sock, int timeout)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(sock->fd, &fds);
	struct timeval ptval;
	ptval.tv_sec = 0;
	ptval.tv_usec = timeout;
	if (timeout == 0)
	{
		return 1;
	}
	if (select(sock->fd+1, &fds, (fd_set *) 0, (fd_set *) 0, &ptval) == -1)
	{
		return -1;
	}
	else if (!FD_ISSET(sock->fd, &fds))
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

int SocketSend(struct socket *sock, int *data, int bytes, int timeout)
{
	int bytesSent = 0;
	if (sock->type == SOCK_STREAM)
	{
		bytesSent = send(sock->fd, data, bytes, 0);
	}
	else if (sock->type == SOCK_DGRAM)
	{
		bytesSent = sendto(sock->fd, (const int *) data, bytes, 0, (struct sockaddr *) &sock->info, sizeof(sock->info));
	}
	return bytesSent - bytes;
}	
