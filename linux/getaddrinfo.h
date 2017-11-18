#ifndef GETADDRINFO_MOD_H
#define GETADDRINFO_MOD_H
// ref: http://www.binarytides.com/dns-query-code-in-c-with-linux-sockets/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#define BUF_RESOLV_LINE_SIZE 100

static char __dns_server[20] = {0};

static int __dns_read_resolve() {
   char buf[BUF_RESOLV_LINE_SIZE] = {0};
   char *r = 0, ch = 0, *cur = 0;
   int state = 0;
   // use cached dns server
   if (__dns_server[0] != 0) {
      return 0;
   }
   FILE* fp = fopen("/etc/resolv.conf", "r");
   if (!fp) {
      strcpy(__dns_server, "8.8.8.8");
      return 1;
   }
   while ((r = fgets(buf, BUF_RESOLV_LINE_SIZE, fp))) {
      unsigned long len = strlen(r);
      // line too long, skip
      if (len >= BUF_RESOLV_LINE_SIZE-1 && r[BUF_RESOLV_LINE_SIZE-2] != '\n') {
         state = -1;
         continue;
      }
      if (state < 0) {
         state = 0;
         continue;
      }
      // parse file
      state = 0;
      // skip tail space and \n
      ch = r[len-1];
      while (len > 0 && (ch == '\n' || ch == ' ' || ch == '\t')) {
         r[len-1] = 0;
         len --;
         ch = r[len-1];
      }
      // parse line by line
      for(unsigned long i = 0; i < len; i++) {
         ch = r[i];
         if (state == 0) {
            // skip head space
            if (ch == ' ' || ch == '\t') continue;
            // skip comment line
            if (ch == '#') break;
            // skip non nameserver line
            if (strncmp("nameserver", r+i, 10)) break;
            i += 10;
            ch = r[i];
            if (ch != ' ' && ch != '\t') break;
            state = 1;
         } else {
            // get dns server ip and return
            // break -> break -> fclose, ensure file close
            if (len - i > 16) break;
            strcpy(__dns_server, &r[i]);
            state = 2;
            break;
         }
      }
      if (state == 2) {
         break;
      }
      state = 0;
   }
   fclose(fp);
   return 0;
}

static int __dns_encode_name(char *dns_name, const char *name, int size) {
   const char *tail = name+size;
   char *cur = dns_name+size+1;
   unsigned char count = 0;
   *cur-- = *tail--; // copy terminate \0
   while(tail != name) {
      if (*tail == '.') {
         *cur = count;
         count = 0;
      } else {
         *cur = *tail;
         count++;
      }
      tail --;
      cur --;
   }
   *cur-- = *tail;
   *cur = count+1;
   return 0;
}

static int __dns_build_query(char *buf, const char *hostname) {
   char *cur = buf;
   int len = strlen(hostname);
   short id = getpid();
   // fill dns header
   *cur++ = ((id / 256) & 0xff); *cur++ = (id & 0xff);
   *cur++ = 0x01; *cur++ = 0x00;
   *cur++ = 0x00; *cur++ = 0x01;
   *cur++ = 0; *cur++ = 0; *cur++ = 0;
   *cur++ = 0; *cur++ = 0; *cur++ = 0;
   __dns_encode_name(cur, hostname, len);
   cur += len+2; // one more space and \0
   *cur++ = 0x00; *cur++ = 0x01;
   *cur++ = 0x00; *cur++ = 0x01;
   return len + 2 + 16;
}

static int __dns_decode_name(char *name, const char *dns_name) {
   const char *head = dns_name;
   char *cur = name;
   while(*head) {
      int sublen = *head++;
      while(sublen-- > 0) *cur++ = *head++;
      *cur++ = '.';
   }
   *cur = 0;
   return 0;
}

static int __dns_parse(const char *buf, int query_len, struct addrinfo **res) {
   unsigned char h, l;
   unsigned int n = 0;
   h = buf[6] & 0xff; l = buf[7] & 0xff;
   n = h*256+l;
   const char *cur = buf+query_len;;
   char *one = 0, *cname = 0, namebuf[1024] = {0};
   struct addrinfo *info = 0, *last = 0, *head = 0;
   struct sockaddr_in *sa = 0;
   for(;n > 0; n--) {
      unsigned int name_offset = 0, name_len = 0, type = 0, klass = 0, rdata_len = 0;
      h = cur[2] & 0xff; l = cur[3] & 0xff;
      type = h*256+l;
      h = cur[10] & 0xff; l = cur[11] & 0xff;
      rdata_len = h*256+l;
      if (type != 1 /* ipv4 */ && rdata_len != 4) {
         cur += rdata_len + 12;
         continue;
      }
      h = cur[0] & 0x3f; l = cur[1] & 0xff;
      name_offset = h*256+l;
      __dns_decode_name(namebuf, buf+name_offset);
      name_len = strlen(namebuf);
      h = cur[4] & 0xff; l = cur[5] & 0xff;
      klass = h*256+l;
      one = (char*)malloc(sizeof(struct addrinfo) + sizeof(struct sockaddr_in) + name_len);
      info = (struct addrinfo*)one;
      sa = (struct sockaddr_in*)(one + sizeof(struct addrinfo));
      cname = one + sizeof(struct addrinfo) + sizeof(struct sockaddr_in);
      strcpy(cname, namebuf);
      info->ai_flags = 0;
      info->ai_family = AF_INET;
      info->ai_socktype = 0;
      info->ai_protocol = 0;
      info->ai_addrlen = sizeof(struct sockaddr_in);
      info->ai_addr = (struct sockaddr *)sa;
      info->ai_canonname = cname;
      info->ai_next = 0;
      sa->sin_family = AF_INET;
      sa->sin_port = 0;
      sa->sin_addr.s_addr = *(unsigned int*)(cur+12);
      if (last) last->ai_next = info;
           else head = info;
      last = info;
      cur += rdata_len + 12;
   }
   *res = head;
   return 0;
}

static int getaddrinfo_mod(const char *hostname, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
   __dns_read_resolve();
   if (__dns_server[0] == 0) {
      return EAI_SYSTEM;
   }
   char buf[4096] = {0};
   int len = __dns_build_query(buf, hostname), t;
   struct sockaddr_in dest;
   int s = socket(AF_INET , SOCK_DGRAM , IPPROTO_UDP);
   dest.sin_family = AF_INET;
   dest.sin_port = htons(53);
   dest.sin_addr.s_addr = inet_addr(__dns_server);
   if(sendto(s, buf, len, 0 ,(struct sockaddr*)&dest, sizeof(dest)) < 0) {
      return EAI_FAIL;
   }
   t = sizeof(dest);
   if(recvfrom (s, buf, 4096, 0, (struct sockaddr*)&dest , (socklen_t*)&t) < 0) {
      return EAI_FAIL;
   }
   close(s);
   __dns_parse(buf, len, res);
   return 0;
}

static void freeaddrinfo_mod(struct addrinfo *res) {
   struct addrinfo *cur = res, *next = 0;
   if (!cur) return;
   do {
      next = cur->ai_next;
      free(cur);
      cur = next;
   } while(cur);
}

#define getaddrinfo getaddrinfo_mod
#define freeaddrinfo freeaddrinfo_mod
#endif
