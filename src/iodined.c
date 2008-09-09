/*
 * Copyright (c) 2006-2008 Bjorn Andersson <flex@kryo.se>, Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <err.h>
#include <grp.h>
#include <time.h>
#include <pwd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <zlib.h>
#include <arpa/nameser.h>
#ifdef DARWIN
#include <arpa/nameser8_compat.h>
#endif

#include "common.h"
#include "dns.h"
#include "encoding.h"
#include "base32.h"
#include "user.h"
#include "login.h"
#include "tun.h"
#include "fw_query.h"
#include "version.h"

static int running = 1;
static char *topdomain;
static char password[33];
static struct encoder *b32;

static int check_ip;
static int my_mtu;
static in_addr_t my_ip;

static int bind_port;
static int debug;

#if !defined(BSD) && !defined(__GLIBC__)
static char *__progname;
#endif

static int read_dns(int, struct query *);
static void write_dns(int, struct query *, char *, int);

static void
sigint(int sig) 
{
	running = 0;
}

static int
ip_cmp(int userid, struct query *q)
{
	struct sockaddr_in *tempin;

	tempin = (struct sockaddr_in *) &(q->from);
	return memcmp(&(users[userid].host), &(tempin->sin_addr), sizeof(struct in_addr));
}

static int
tunnel_tun(int tun_fd, int dns_fd)
{
	unsigned long outlen;
	struct ip *header;
	char out[64*1024];
	char in[64*1024];
	int userid;
	int read;

	if ((read = read_tun(tun_fd, in, sizeof(in))) <= 0)
		return 0;
	
	/* find target ip in packet, in is padded with 4 bytes TUN header */
	header = (struct ip*) (in + 4);
	userid = find_user_by_ip(header->ip_dst.s_addr);
	if (userid < 0)
		return 0;

	outlen = sizeof(out);
	compress2((uint8_t*)out, &outlen, (uint8_t*)in, read, 9);

	/* if another packet is queued, throw away this one. TODO build queue */
	if (users[userid].outpacket.len == 0) {
		memcpy(users[userid].outpacket.data, out, outlen);
		users[userid].outpacket.len = outlen;
		if (users[userid].q.id != 0) {
			/* If delayed response is kept, send reply immediately */
			write_dns(dns_fd, &(users[userid].q), users[userid].outpacket.data, users[userid].outpacket.len);
			users[userid].outpacket.len = 0;
			users[userid].q.id = 0;
			return 0;
		}
		return outlen;
	} else {
		return 0;
	}
}

typedef enum {
	VERSION_ACK,
	VERSION_NACK,
	VERSION_FULL
} version_ack_t;

static void
send_version_response(int fd, version_ack_t ack, uint32_t payload, int userid, struct query *q)
{
	char out[9];
	
	switch (ack) {
	case VERSION_ACK:
		strncpy(out, "VACK", sizeof(out));
		break;
	case VERSION_NACK:
		strncpy(out, "VNAK", sizeof(out));
		break;
	case VERSION_FULL:
		strncpy(out, "VFUL", sizeof(out));
		break;
	}
	
	out[4] = ((payload >> 24) & 0xff);
	out[5] = ((payload >> 16) & 0xff);
	out[6] = ((payload >> 8) & 0xff);
	out[7] = ((payload) & 0xff);
	out[8] = userid & 0xff;

	write_dns(fd, q, out, sizeof(out));
}

static void
handle_null_request(int tun_fd, int dns_fd, struct query *q, int domain_len)
{
	struct in_addr tempip;
	struct ip *hdr;
	unsigned long outlen;
	char in[512];
	char logindata[16];
	char out[64*1024];
	char unpacked[64*1024];
	char *tmp[2];
	int userid;
	int touser;
	int version;
	int code;
	int read;

	userid = -1;

	memcpy(in, q->name, MIN(domain_len, sizeof(in)));

	if(in[0] == 'V' || in[0] == 'v') {
		read = unpack_data(unpacked, sizeof(unpacked), &(in[1]), domain_len - 1, b32);
		/* Version greeting, compare and send ack/nak */
		if (read > 4) { 
			/* Received V + 32bits version */
			version = (((unpacked[0] & 0xff) << 24) |
					   ((unpacked[1] & 0xff) << 16) |
					   ((unpacked[2] & 0xff) << 8) |
					   ((unpacked[3] & 0xff)));
		}

		if (version == VERSION) {
			userid = find_available_user();
			if (userid >= 0) {
				struct sockaddr_in *tempin;

				users[userid].seed = rand();
				/* Store remote IP number */
				tempin = (struct sockaddr_in *) &(q->from);
				memcpy(&(users[userid].host), &(tempin->sin_addr), sizeof(struct in_addr));
				
				memcpy(&(users[userid].q), q, sizeof(struct query));
				users[userid].encoder = get_base32_encoder();
				send_version_response(dns_fd, VERSION_ACK, users[userid].seed, userid, q);
				users[userid].q.id = 0;
			} else {
				/* No space for another user */
				send_version_response(dns_fd, VERSION_FULL, USERS, 0, q);
			}
		} else {
			send_version_response(dns_fd, VERSION_NACK, VERSION, 0, q);
		}
		return;
	} else if(in[0] == 'L' || in[0] == 'l') {
		read = unpack_data(unpacked, sizeof(unpacked), &(in[1]), domain_len - 1, b32);
		/* Login phase, handle auth */
		userid = unpacked[0];
		if (userid < 0 || userid >= USERS) {
			write_dns(dns_fd, q, "BADIP", 5);
			return; /* illegal id */
		}
		users[userid].last_pkt = time(NULL);
		login_calculate(logindata, 16, password, users[userid].seed);

		if (check_ip && ip_cmp(userid, q) != 0) {
			write_dns(dns_fd, q, "BADIP", 5);
		} else {
			if (read >= 18 && (memcmp(logindata, unpacked+1, 16) == 0)) {
				/* Login ok, send ip/mtu info */

				tempip.s_addr = my_ip;
				tmp[0] = strdup(inet_ntoa(tempip));
				tempip.s_addr = users[userid].tun_ip;
				tmp[1] = strdup(inet_ntoa(tempip));

				read = snprintf(out, sizeof(out), "%s-%s-%d", 
						tmp[0], tmp[1], my_mtu);

				write_dns(dns_fd, q, out, read);
				q->id = 0;

				free(tmp[1]);
				free(tmp[0]);
			} else {
				write_dns(dns_fd, q, "LNAK", 4);
			}
		}
		return;
	} else if(in[0] == 'P' || in[0] == 'p') {
		read = unpack_data(unpacked, sizeof(unpacked), &(in[1]), domain_len - 1, b32);
		/* Ping packet, store userid */
		userid = unpacked[0];
		if (userid < 0 || userid >= USERS || ip_cmp(userid, q) != 0) {
			write_dns(dns_fd, q, "BADIP", 5);
			return; /* illegal id */
		}
		if (users[userid].q.id != 0) {
			/* If delayed response is kept, send empty reply before overwriting */
			write_dns(dns_fd, &(users[userid].q), users[userid].outpacket.data, users[userid].outpacket.len);
			users[userid].outpacket.len = 0;
			users[userid].q.id = 0;
		}
		memcpy(&(users[userid].q), q, sizeof(struct query));
		users[userid].last_pkt = time(NULL);
	} else if(in[0] == 'Z' || in[0] == 'z') {
		/* Case conservation check */

		/* Reply with received hostname as data */
		write_dns(dns_fd, q, in, domain_len);
		return;
	} else if((in[0] >= '0' && in[0] <= '9')
			|| (in[0] >= 'a' && in[0] <= 'f')
			|| (in[0] >= 'A' && in[0] <= 'F')) {
		if ((in[0] >= '0' && in[0] <= '9'))
			code = in[0] - '0';
		if ((in[0] >= 'a' && in[0] <= 'f'))
			code = in[0] - 'a' + 10;
		if ((in[0] >= 'A' && in[0] <= 'F'))
			code = in[0] - 'A' + 10;

		userid = code >> 1;
		if (userid < 0 || userid >= USERS) {
			write_dns(dns_fd, q, "BADIP", 5);
			return; /* illegal id */
		}

		/* Check sending ip number */
		if (check_ip && ip_cmp(userid, q) != 0) {
			write_dns(dns_fd, q, "BADIP", 5);
		} else {
			/* decode with this users encoding */
			read = unpack_data(unpacked, sizeof(unpacked), &(in[1]), domain_len - 1, 
					   users[userid].encoder);

			users[userid].last_pkt = time(NULL);
			if (users[userid].q.id != 0) {
				/* If delayed response is kept, send empty reply before overwriting */
				write_dns(dns_fd, &(users[userid].q), users[userid].outpacket.data, users[userid].outpacket.len);
				users[userid].outpacket.len = 0;
				users[userid].q.id = 0;
			}
			memcpy(&(users[userid].q), q, sizeof(struct query));
			memcpy(users[userid].inpacket.data + users[userid].inpacket.offset, unpacked, read);
			users[userid].inpacket.len += read;
			users[userid].inpacket.offset += read;

			if (code & 1) {
				outlen = sizeof(out);
				uncompress((uint8_t*)out, &outlen, 
						   (uint8_t*)users[userid].inpacket.data, users[userid].inpacket.len);

				hdr = (struct ip*) (out + 4);
				touser = find_user_by_ip(hdr->ip_dst.s_addr);

				if (touser == -1) {
					/* send the uncompressed packet to tun device */
					write_tun(tun_fd, out, outlen);
				} else {
					/* send the compressed packet to other client
					 * if another packet is queued, throw away this one. TODO build queue */
					if (users[touser].outpacket.len == 0) {
						memcpy(users[touser].outpacket.data, users[userid].inpacket.data, users[userid].inpacket.len);
						users[touser].outpacket.len = users[userid].inpacket.len;
					}
				}
				users[userid].inpacket.len = users[userid].inpacket.offset = 0;
			}
		}
	}
	/* userid must be set for a reply to be sent */
	if (userid >= 0 && userid < USERS && ip_cmp(userid, q) == 0 && users[userid].outpacket.len > 0) {
		write_dns(dns_fd, q, users[userid].outpacket.data, users[userid].outpacket.len);
		users[userid].outpacket.len = 0;
		users[userid].q.id = 0;
	}
}

static void
forward_query(int bind_fd, struct query *q)
{
	char buf[64*1024];
	int len;
	struct fw_query fwq;
	struct sockaddr_in *myaddr;
	in_addr_t newaddr;

	len = dns_encode(buf, sizeof(buf), q, QR_QUERY, q->name, strlen(q->name));

	/* Store sockaddr for q->id */
	memcpy(&(fwq.addr), &(q->from), q->fromlen);
	fwq.addrlen = q->fromlen;
	fwq.id = q->id;
	fw_query_put(&fwq);

	newaddr = inet_addr("127.0.0.1");
	myaddr = (struct sockaddr_in *) &(q->from);
	memcpy(&(myaddr->sin_addr), &newaddr, sizeof(in_addr_t));
	myaddr->sin_port = htons(bind_port);
	
	if (debug >= 1) {
		printf("TX: send query %u to DNS (port %d)\n", (q->id & 0xffff), bind_port);
	}

	if (sendto(bind_fd, buf, len, 0, (struct sockaddr*)&q->from, q->fromlen) <= 0) {
		warn("forward query error");
	}
}
  
static int
tunnel_bind(int bind_fd, int dns_fd)
{
	char packet[64*1024];
	struct sockaddr_in from;
	socklen_t fromlen;
	struct fw_query *query;
	short id;
	int r;

	fromlen = sizeof(struct sockaddr);
	r = recvfrom(bind_fd, packet, sizeof(packet), 0, (struct sockaddr*)&from, &fromlen);

	if (r <= 0)
		return 0;

	id = dns_get_id(packet, r);
	
	if (debug >= 1) {
		printf("RX: Got response on query %u from DNS\n", (id & 0xFFFF));
	}

	/* Get sockaddr from id */
	fw_query_get(id, &query);
	if (!query && debug >= 1) {
		printf("Lost sender of id %u, dropping reply\n", (id & 0xFFFF));
		return 0;
	}

	if (debug >= 1) {
		struct sockaddr_in *in;
		in = (struct sockaddr_in *) &(query->addr);
		printf("TX: client %s id %u, %d bytes\n",
			inet_ntoa(in->sin_addr), (id & 0xffff), r);
	}
	
	if (sendto(dns_fd, packet, r, 0, (const struct sockaddr *) &(query->addr), query->addrlen) <= 0) {
		warn("forward reply error");
	}

	return 0;
}

static int
tunnel_dns(int tun_fd, int dns_fd, int bind_fd)
{
	struct query q;
	int read;
	char *domain;
	int domain_len;
	int inside_topdomain;

	if ((read = read_dns(dns_fd, &q)) <= 0)
		return 0;

	if (debug >= 1) {
		struct sockaddr_in *tempin;
		tempin = (struct sockaddr_in *) &(q.from);
		printf("RX: client %s, type %d, name %s\n", 
			inet_ntoa(tempin->sin_addr), q.type, q.name);
	}
	
	domain = strstr(q.name, topdomain);
	inside_topdomain = 0;
	if (domain) {
		domain_len = (int) (domain - q.name); 
		if (domain_len + strlen(topdomain) == strlen(q.name)) {
			inside_topdomain = 1;
		}
	}
	
	if (inside_topdomain) {
		/* This is a query we can handle */
		switch (q.type) {
		case T_NULL:
			handle_null_request(tun_fd, dns_fd, &q, domain_len);
			break;
		default:
			break;
		}
	} else {
		/* Forward query to other port ? */
		if (bind_fd) {
			forward_query(bind_fd, &q);
		}
	}
	return 0;
}

static int
tunnel(int tun_fd, int dns_fd, int bind_fd)
{
	struct timeval tv;
	fd_set fds;
	int i;

	while (running) {
		int maxfd;

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&fds);

		FD_SET(dns_fd, &fds);
		maxfd = dns_fd;

		if (bind_fd) {
			/* wait for replies from real DNS */
			FD_SET(bind_fd, &fds);
			maxfd = MAX(bind_fd, maxfd);
		}

		/* TODO : use some kind of packet queue */
		if(!all_users_waiting_to_send()) {
			FD_SET(tun_fd, &fds);
			maxfd = MAX(tun_fd, maxfd);
		}

		i = select(maxfd + 1, &fds, NULL, NULL, &tv);
		
		if(i < 0) {
			if (running) 
				warn("select");
			return 1;
		}
	
		if(FD_ISSET(tun_fd, &fds)) {
			tunnel_tun(tun_fd, dns_fd);
			continue;
		}
		if(FD_ISSET(dns_fd, &fds)) {
			tunnel_dns(tun_fd, dns_fd, bind_fd);
			continue;
		} 
		if(FD_ISSET(bind_fd, &fds)) {
			tunnel_bind(bind_fd, dns_fd);
			continue;
		}
	}

	return 0;
}

static int
read_dns(int fd, struct query *q)
{
	struct sockaddr_in from;
	socklen_t addrlen;
	char packet[64*1024];
	char address[96];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	int r;

	addrlen = sizeof(struct sockaddr);
	iov.iov_base = packet;
	iov.iov_len = sizeof(packet);

	msg.msg_name = (caddr_t) &from;
	msg.msg_namelen = (unsigned) addrlen;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = address;
	msg.msg_controllen = sizeof(address);
	msg.msg_flags = 0;
	
	r = recvmsg(fd, &msg, 0);

	if (r > 0) {
		dns_decode(NULL, 0, q, QR_QUERY, packet, r);
		memcpy((struct sockaddr*)&q->from, (struct sockaddr*)&from, addrlen);
		q->fromlen = addrlen;
		
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) { 
			if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == DSTADDR_SOCKOPT) { 
				q->destination = *dstaddr(cmsg); 
				break;
			} 
		}

		return strlen(q->name);
	} else if (r < 0) { 
		/* Error */
		warn("read dns");
	}

	return 0;
}

static void
write_dns(int fd, struct query *q, char *data, int datalen)
{
	char buf[64*1024];
	int len;

	len = dns_encode(buf, sizeof(buf), q, QR_ANSWER, data, datalen);
	
	if (debug >= 1) {
		struct sockaddr_in *tempin;
		tempin = (struct sockaddr_in *) &(q->from);
		printf("TX: client %s, type %d, name %s, %d bytes data\n", 
			inet_ntoa(tempin->sin_addr), q->type, q->name, datalen);
	}

	sendto(fd, buf, len, 0, (struct sockaddr*)&q->from, q->fromlen);
}

static void
usage() {
	extern char *__progname;

	printf("Usage: %s [-v] [-h] [-c] [-s] [-f] [-D] [-u user] [-t chrootdir] [-d device] [-m mtu] "
		"[-l ip address to listen on] [-p port] [-b port] [-P password]"
		" tunnel_ip topdomain\n", __progname);
	exit(2);
}

static void
help() {
	extern char *__progname;

	printf("iodine IP over DNS tunneling server\n");
	printf("Usage: %s [-v] [-h] [-c] [-s] [-f] [-D] [-u user] [-t chrootdir] [-d device] [-m mtu] "
		"[-l ip address to listen on] [-p port] [-b port] [-P password]"
		" tunnel_ip topdomain\n", __progname);
	printf("  -v to print version info and exit\n");
	printf("  -h to print this help and exit\n");
	printf("  -c to disable check of client IP/port on each request\n");
	printf("  -s to skip creating and configuring the tun device which then has to be created manually\n");
	printf("  -f to keep running in foreground\n");
	printf("  -D to increase debug level\n");
	printf("  -u name to drop privileges and run as user 'name'\n");
	printf("  -t dir to chroot to directory dir\n");
	printf("  -d device to set tunnel device name\n");
	printf("  -m mtu to set tunnel device mtu\n");
	printf("  -l ip address to listen on for incoming dns traffic (default 0.0.0.0)\n");
	printf("  -p port to listen on for incoming dns traffic (default 53)\n");
	printf("  -b port to forward normal DNS queries to (on localhost)\n");
	printf("  -P password used for authentication (max 32 chars will be used)\n");
	printf("tunnel_ip is the IP number of the local tunnel interface.\n");
	printf("topdomain is the FQDN that is delegated to this server.\n");
	exit(0);
}

static void
version() {
	char *svnver;
	svnver = "$Rev$ from $Date$";
	printf("iodine IP over DNS tunneling server\n");
	printf("SVN version: %s\n", svnver);
	exit(0);
}

int
main(int argc, char **argv)
{
	in_addr_t listen_ip;
	struct passwd *pw;
	int foreground;
	char *username;
	char *newroot;
	char *device;
	int dnsd_fd;
	int tun_fd;

	/* settings for forwarding normal DNS to 
	 * local real DNS server */
	int bind_fd;
	int bind_enable;
	
	int choice;
	int port;
	int mtu;
	int skipipconfig;

	username = NULL;
	newroot = NULL;
	device = NULL;
	foreground = 0;
	bind_enable = 0;
	bind_fd = 0;
	mtu = 1024;
	listen_ip = INADDR_ANY;
	port = 53;
	check_ip = 1;
	skipipconfig = 0;
	debug = 0;

	b32 = get_base32_encoder();

#if !defined(BSD) && !defined(__GLIBC__)
	__progname = strrchr(argv[0], '/');
	if (__progname == NULL)
		__progname = argv[0];
	else
		__progname++;
#endif

	memset(password, 0, sizeof(password));
	srand(time(NULL));
	fw_query_init();
	
	while ((choice = getopt(argc, argv, "vcsfhDu:t:d:m:l:p:b:P:")) != -1) {
		switch(choice) {
		case 'v':
			version();
			break;
		case 'c':
			check_ip = 0;
			break;
		case 's':
			skipipconfig = 1;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'h':
			help();
			break;
		case 'D':
			debug++;
			break;
		case 'u':
			username = optarg;
			break;
		case 't':
			newroot = optarg;
			break;
		case 'd':
			device = optarg;
			break;
		case 'm':
			mtu = atoi(optarg);
			break;
		case 'l':
			listen_ip = inet_addr(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'b':
			bind_enable = 1;
			bind_port = atoi(optarg);
			break;
		case 'P':
			strncpy(password, optarg, sizeof(password));
			password[sizeof(password)-1] = 0;
			
			/* XXX: find better way of cleaning up ps(1) */
			memset(optarg, 0, strlen(optarg)); 
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (geteuid() != 0) {
		warnx("Run as root and you'll be happy.\n");
		usage();
	}

	if (argc != 2) 
		usage();

	topdomain = strdup(argv[1]);
	if(strlen(topdomain) <= 128) {
		if(check_topdomain(topdomain)) {
			warnx("Topdomain contains invalid characters.\n");
			usage();
		}
	} else {
		warnx("Use a topdomain max 128 chars long.\n");
		usage();
	}

	if (username != NULL) {
		if ((pw = getpwnam(username)) == NULL) {
			warnx("User %s does not exist!\n", username);
			usage();
		}
	}

	if (mtu <= 0) {
		warnx("Bad MTU given.\n");
		usage();
	}
	
	if(port < 1 || port > 65535) {
		warnx("Bad port number given.\n");
		usage();
	}
	
	if(bind_enable) {
		if (bind_port < 1 || bind_port > 65535 || bind_port == port) {
			warnx("Bad DNS server port number given.\n");
			usage();
			/* NOTREACHED */
		}
		printf("Requests for domains outside of %s will be forwarded to port %d\n",
			topdomain, bind_port);
	}
	
	if (port != 53) {
		printf("ALERT! Other dns servers expect you to run on port 53.\n");
		printf("You must manually forward port 53 to port %d for things to work.\n", port);
	}

	if (debug) {
		printf("Debug level %d enabled, will stay in foreground.\n", debug);
		printf("Add more -D switches to set higher debug level.\n");
		foreground = 1;
	}

	if (listen_ip == INADDR_NONE) {
		warnx("Bad IP address to listen on.\n");
		usage();
	}

	if (strlen(password) == 0)
		read_password(password, sizeof(password));

	if ((tun_fd = open_tun(device)) == -1)
		goto cleanup0;
	if (!skipipconfig)
		if (tun_setip(argv[0]) != 0 || tun_setmtu(mtu) != 0)
			goto cleanup1;
	if ((dnsd_fd = open_dns(port, listen_ip)) == -1) 
		goto cleanup2;
	if (bind_enable)
		if ((bind_fd = open_dns(0, INADDR_ANY)) == -1)
			goto cleanup3;

	my_ip = inet_addr(argv[0]);
	my_mtu = mtu;
	init_users(my_ip);

	printf("Listening to dns for domain %s\n", topdomain);

	if (foreground == 0) 
		do_detach();
	
	if (newroot != NULL)
		do_chroot(newroot);

	signal(SIGINT, sigint);
	if (username != NULL) {
		gid_t gids[1];
		gids[0] = pw->pw_gid;
		if (setgroups(1, gids) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			warnx("Could not switch to user %s!\n", username);
			usage();
		}
	}
	
	tunnel(tun_fd, dnsd_fd, bind_fd);

cleanup3:
	close_dns(bind_fd);
cleanup2:
	close_dns(dnsd_fd);
cleanup1:
	close_tun(tun_fd);	
cleanup0:

	return 0;
}
