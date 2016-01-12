/* Slice-side code to allocate tuntap interface in root slice
 * Based on bmsocket.c
 *  Thom Haddow - 08/10/09
 * Reworked
 *  k.edeline - 2015
 * Call tun_alloc() with IFFTUN or IFFTAP as an argument to get back fd to
 * new tuntap interface. Interface name can be acquired via TUNGETIFF ioctl.
 */

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "sock.h"

/**
 * \def VSYS_TUNTAP
 * \brief planetlab vsys control file descriptor
 */
#define VSYS_TUNTAP "/vsys/fd_tuntap.control"

/**
 * \def VSYS_VIFUP_IN 
 * \brief planetlab vsys control file descriptor
 */
#define VSYS_VIFUP_IN "/vsys/vif_up.in"

/**
 * \def VSYS_VIFUP_OUT
 * \brief planetlab vsys control file descriptor
 */
#define VSYS_VIFUP_OUT "/vsys/vif_up.out"

struct in6_ifreq {
    struct in6_addr ifr6_addr;
    __u32 ifr6_prefixlen;
    unsigned int ifr6_ifindex;
};

static int tun_alloc(const char *ip, const char *prefix, char *dev, int common);
static int tun_alloc6(const char *ip, const char *prefix, char *dev, int common);
static int tun_alloc46(const char *ip, const char *prefix, char *dev, int common);
static int tun_alloc_old(char *dev);

/**
 * \fn int tun_alloc_pl(int iftype, char *if_name)
 * \brief Allocate a tun interface on a PlanetLab
 * VM.
 *
 * \param iftype The interface type (IFFTUN or IFFTAP)
 * \param if_name buffer to be filled with newly created if name.
 * \return fd
 */ 
static int tun_alloc_pl(int iftype, char *if_name);

/* Reads vif FD from "fd", writes interface name to vif_name, and returns vif FD.
 * vif_name should be IFNAMSIZ chars long. */
int receive_vif_fd(int fd, char *vif_name)
{
	struct msghdr msg;
	struct iovec iov;
	int rv;
	size_t ccmsg[CMSG_SPACE(sizeof(int)) / sizeof(size_t)];
	struct cmsghdr *cmsg;

    /* Use IOV to read interface name */
	iov.iov_base = vif_name;
	iov.iov_len = IFNAMSIZ;

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	/* old BSD implementations should use msg_accrights instead of
	 * msg_control; the interface is different. */
	msg.msg_control = ccmsg;
	msg.msg_controllen = sizeof(ccmsg);

	while(((rv = recvmsg(fd, &msg, 0)) == -1) && errno == EINTR);
	if (rv == -1) {
		perror("recvmsg");
		return -1;
	}
	if(!rv) {
		/* EOF */
		return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg->cmsg_type == SCM_RIGHTS) {
		fprintf(stderr, "got control message of unknown type %d\n",
			cmsg->cmsg_type);
		return -1;
	}
	return *(int*)CMSG_DATA(cmsg);
}

int tun_alloc_pl(int iftype, char *if_name) {
    int control_fd;
    struct sockaddr_un addr;
    int remotefd;

    control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control_fd == -1) 
        die("Could not create UNIX socket\n");

    memset(&addr, 0, sizeof(struct sockaddr_un));
    /* Clear structure */
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, VSYS_TUNTAP,
            sizeof(addr.sun_path) - 1);

    if (connect(control_fd, (struct sockaddr *) &addr,
                sizeof(struct sockaddr_un)) == -1) 
        die("Could not connect to Vsys control socket");

    /* passing type param */
    if (send(control_fd, &iftype, sizeof(iftype), 0) != sizeof(iftype)) 
        die("Could not send paramater to Vsys control socket");

    remotefd = receive_vif_fd(control_fd, if_name);
    return remotefd;
}

char *create_tun_pl(const char *ip, const char *prefix, int *tun_fds) {
   char *if_name = malloc(IFNAMSIZ);

   FILE *in;
   FILE *out;
   char errbuff[4096];
   memset(errbuff, 0, 4096);

   int tun_fd = tun_alloc_pl(IFF_TUN, if_name);
   if (tun_fds) *tun_fds = tun_fd;

   debug_print("allocated tun device: %s fd=%d\n", if_name, tun_fd);

   in = fopen (VSYS_VIFUP_IN, "a");
   if (!in) 
     die("fopen VSYS_VIFUP_IN");

   out = fopen (VSYS_VIFUP_OUT, "r");
   if (!out) 
      die("fopen VSYS_VIFUP_OUT");
   
   // send input to process
   //if (nat)
   //   fprintf (in, "%s\n%s\n%s\nsnat=1\n", if_name, ip, prefix);
   // else
   fprintf (in, "%s\n%s\n%s\n\n", if_name, ip, prefix);

   // close pipe to indicate end parameter passing and flush the fifo
   fclose (in);

   if (fread((void*)errbuff, 4096, 1, out) && strcmp(errbuff, ""))
      debug_print("%s\n",errbuff);

   fclose (out);
   return if_name;
}

/*
 *
 *
 * @param dev choose tun itf name
 * @param tun_fds empty int pointer to point to tun itf fd
 * @return if_name    --tcp-saddr=192.168.2.1
 */
char *create_tun(const char *ip, const char *prefix, char *dev, int *tun_fds) {

   int   fd;
   char *if_name = malloc(IFNAMSIZ);
   
   if (dev) {
      if ((fd = tun_alloc(ip, prefix, dev, 0)) >= 0) {
         strcpy(if_name, dev);
         goto succ;
      } else goto err;
      
   }

   for (int i=0; i<99; i++) {
      sprintf(if_name, "tun%d", i);
      if ((fd = tun_alloc(ip, prefix, if_name, 1)) >= 0) {
         break;
      } else goto err;
   }

succ:
   debug_print("%s interface created %d\n", if_name, fd);
   if (tun_fds) 
      *tun_fds = fd;
   return if_name;
err:
   return 0;
}

// ip tuntap add mode tun dev tun1
// ifconfig tun1 up OR ip link set tun0 up
// ip addr add 192.168.1.2/24 dev tun1 
// ip route add 192.168.1.2 dev tun1 (useless ?)
//2001:412:abcd::/48 - Local IPv6 network
// 192.168.2.0/24 - Tunnel IPv4 network
//2001:412:abcd:2::/64 - Tunnel IPv6 network
int tun_alloc46(const char *ip, const char *prefix, char *dev, int common) {
   struct ifreq ifr;
   struct in6_ifreq ifr6;
   int fd, err;
   
   if (common) {
      if((fd = open("/dev/net/tun", O_RDWR)) < 0 ) 
         die("err opening tun fd\n");
   } else {
      char tunname[14];
      sprintf(tunname, "/dev/%s", dev);
      if((fd = open(tunname, O_RDWR)) < 0 ) 
         //      mknod /dev/tun1 c 10 200
         die("err opening tun fd\n");
      return fd;
   }

   memset(&ifr, 0, sizeof(ifr));
   ifr.ifr_flags = IFF_TUN | IFF_NO_PI; 
   if( *dev )
      strncpy(ifr.ifr_name, dev, IFNAMSIZ);

   if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ) 
      die("ioctl\n");
   strcpy(dev, ifr.ifr_name);

   // Create a socket
   int s4, s6;
   if ( (s4 = socket(AF_INET, SOCK_DGRAM, 0)) < 0) 
      die("socket");
   if ( (s6 = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) 
      die("socket");

   // Get interface flags
   if (ioctl(s4, SIOCGIFFLAGS, &ifr) < 0) 
      die("cannot get interface flags");

   // Turn on interface
   ifr.ifr_flags |= IFF_UP;
   if (ioctl(s4, SIOCSIFFLAGS, &ifr) < 0) 
      die("ioctl ifup");

   // Set interface address
   struct sockaddr_in  tun_addr4;
   memset((char *) &tun_addr4, 0, sizeof(tun_addr4));
   tun_addr4.sin_family = AF_INET;
   tun_addr4.sin_addr.s_addr = htonl(inet_network(ip));
   memcpy(&ifr.ifr_addr, &tun_addr4, sizeof(struct sockaddr));

   if (ioctl(s4, SIOCSIFADDR, &ifr) < 0) 
      die("cannot set IP address. ");

   //if (ioctl(s6, SIOCGIFFLAGS, &ifr) < 0) 
   //   die("cannot get interface flags");

   char net_prefix_cmd[128]; // TODO:sanitize args or set mask differently, or remove if useless
   sprintf(net_prefix_cmd, "ip addr add %s/%s dev %s", ip, prefix, dev);
   if (system(net_prefix_cmd) < 0) 
      die("tun prefix");


   ip = "2001:412:abcd:2::";
   // Set interface address
   struct sockaddr_in6  tun_addr6;
   memset(&tun_addr6, 0, sizeof(tun_addr6));
   tun_addr6.sin6_family = AF_INET6;
   if(inet_pton(AF_INET6, ip, (void *)&tun_addr6.sin6_addr) <= 0) 
        die("Bad address\n");
   memcpy((char *) &ifr6.ifr6_addr, (char *) &tun_addr6.sin6_addr,
               sizeof(struct in6_addr));

    if (ioctl(s6, SIOGIFINDEX, &ifr) < 0) 
        die("SIOGIFINDEX");
    ifr6.ifr6_ifindex   = ifr.ifr_ifindex;
    ifr6.ifr6_prefixlen = strtol(prefix, NULL, 10);
    if (ioctl(s6, SIOCSIFADDR, &ifr6) < 0) 
        die("SIOCSIFADDR");


    /*ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
      die("SIOCSIFFLAGS");*/
   close(s4);close(s6);
   return fd;
}              

int tun_alloc6(const char *ip, const char *prefix, char *dev, int common) {
   struct ifreq ifr;
   struct in6_ifreq ifr6;
   int fd, err;
   ip = "2001:412:abcd:2::";
   if (common) {
      if((fd = open("/dev/net/tun", O_RDWR)) < 0 ) 
         die("err opening tun fd\n");
   } else {
      char tunname[14];
      sprintf(tunname, "/dev/%s", dev);
      if((fd = open(tunname, O_RDWR)) < 0 ) 
         //      mknod /dev/tun1 c 10 200
         die("err opening tun fd\n");
      return fd;
   }

   memset(&ifr, 0, sizeof(ifr));
   ifr.ifr_flags = IFF_TUN | IFF_NO_PI; 
   if( *dev )
      strncpy(ifr.ifr_name, dev, IFNAMSIZ);
   if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ) 
      die("ioctl\n");
   strcpy(dev, ifr.ifr_name);

   // Create a socket
   int s;
   if ( (s = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) 
      die("socket");
   // Get interface flags
   if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) 
      die("cannot get interface flags");

   // Turn on interface
   //ifr.ifr_flags |= IFF_UP;
   //if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
   //   die("ioctl ifup");
   //}

   // Set interface address
   struct sockaddr_in6  tun_addr;
   memset(&tun_addr, 0, sizeof(tun_addr));
   tun_addr.sin6_family = AF_INET6;
   //tun_addr.sin6_addr.s6_addr = htonl(inet_network(ip));
   if(inet_pton(AF_INET6, ip, (void *)&tun_addr.sin6_addr) <= 0) 
        die("Bad address\n");
   memcpy((char *) &ifr6.ifr6_addr, (char *) &tun_addr.sin6_addr,
               sizeof(struct in6_addr));

    if (ioctl(s, SIOGIFINDEX, &ifr) < 0) 
        die("SIOGIFINDEX");
    ifr6.ifr6_ifindex   = ifr.ifr_ifindex;
    ifr6.ifr6_prefixlen = strtol(prefix, NULL, 10);
    if (ioctl(s, SIOCSIFADDR, &ifr6) < 0) 
        die("SIOCSIFADDR");

    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;

    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
      die("SIOCSIFFLAGS");

   
   /*char net_prefix_cmd[148]; 
   sprintf(net_prefix_cmd, "sudo ip -6 addr add %s/64 dev %s", ip, dev);
   if (system(net_prefix_cmd) < 0) 
      die("tun prefix");
    */
   close(s);
   return fd;
} 


int tun_alloc(const char *ip, const char *prefix, char *dev, int common) {
   struct ifreq ifr;
   int fd, err;
   
   if (common) {
      if((fd = open("/dev/net/tun", O_RDWR)) < 0 ) 
         die("err opening tun fd\n");
   } else {
      char tunname[14];
      sprintf(tunname, "/dev/%s", dev);
      if((fd = open(tunname, O_RDWR)) < 0 ) 
         //      mknod /dev/tun1 c 10 200
         die("err opening tun fd\n");
      return fd;
   }

   memset(&ifr, 0, sizeof(ifr));
   ifr.ifr_flags = IFF_TUN | IFF_NO_PI; 
   if( *dev )
      strncpy(ifr.ifr_name, dev, IFNAMSIZ);

   if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ) 
      die("ioctl\n");
   strcpy(dev, ifr.ifr_name);

   // Create a socket
   int s;
   if ( (s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) 
      die("socket");

   // Get interface flags
   if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) 
      die("cannot get interface flags");

   // Turn on interface
   ifr.ifr_flags |= IFF_UP;
   if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) 
      die("ioctl ifup");

   // Set interface address
   struct sockaddr_in  tun_addr;
   bzero((char *) &tun_addr, sizeof(tun_addr));
   tun_addr.sin_family = AF_INET;
   tun_addr.sin_addr.s_addr = htonl(inet_network(ip));
   memcpy(&ifr.ifr_addr, &tun_addr, sizeof(struct sockaddr));

   if (ioctl(s, SIOCSIFADDR, &ifr) < 0) 
      die("cannot set IP address. ");

   char net_prefix_cmd[128]; // TODO:sanitize args or set mask differently, or remove if useless
   sprintf(net_prefix_cmd, "ip addr add %s/%s dev %s", ip, prefix, dev);
   if (system(net_prefix_cmd) < 0) 
      die("tun prefix");

   close(s);
   return fd;
}              

int tun_alloc_mq(char *dev, int queues, int *fds) {
   struct ifreq ifr;
   int fd, err, i;

   if (!dev)
       return -1;

   memset(&ifr, 0, sizeof(ifr));
   /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
    *        IFF_TAP   - TAP device
    *
    *        IFF_NO_PI - Do not provide packet information
    *        IFF_MULTI_QUEUE - Create a queue of multiqueue device
    */
   ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_MULTI_QUEUE;
   strcpy(ifr.ifr_name, dev);

   for (i = 0; i < queues; i++) {
       if ((fd = open("/dev/net/tun", O_RDWR)) < 0)
          goto err;
       err = ioctl(fd, TUNSETIFF, (void *)&ifr);
       if (err) {
          close(fd);
          goto err;
       }
       fds[i] = fd;
   }

   return 0;
err:
   for (--i; i >= 0; i--)
       close(fds[i]);
   return err;
}

int tun_set_queue(int fd, int enable) {
   struct ifreq ifr;

   memset(&ifr, 0, sizeof(ifr));

   if (enable)
      ifr.ifr_flags = IFF_ATTACH_QUEUE;
   else
      ifr.ifr_flags = IFF_DETACH_QUEUE;

   return ioctl(fd, TUNSETQUEUE, (void *)&ifr);
}

