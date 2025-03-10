#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <libnet.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

struct _site{
	char url[80];
	int len;
};

struct _site block_site[600000];
int total_site = 0;

int my_strcmp_eq(unsigned char* A, unsigned char* B, int len){
	for(int i=0;i<len;i++){
		if(A[i] != B[i])
			return 1;
	}
	return 0;
}

int should_block(unsigned char * buf, int offset, int len){
	printf("[+] ");
	for(int i=0;i<len;i++){
		printf("%c", buf[offset+i]);
	}
	for(int i=0;i<total_site;i++){
		if(len != block_site[i].len)	continue;
		if(my_strcmp_eq(buf+offset, block_site[i].url, len))	continue;
		printf(" is blocked(%d)\n", i);
		return 1;
	}
	printf(" is not blocked\n");
	return 0;
}

int is_block(unsigned char* buf, int size) {
	//printf("%d ", size);
	int ipv4_hdr_size = (buf[0] & 0xf) * 4;
	//printf("%d ", ipv4_hdr_size);
	if(ipv4_hdr_size < LIBNET_IPV4_H || size < ipv4_hdr_size+LIBNET_TCP_H){
		return 0;
	}
	int tcp_hdr_size =  (buf[ipv4_hdr_size+12] >> 4) * 4;
	//printf("%d\n", tcp_hdr_size);
	if(tcp_hdr_size < LIBNET_TCP_H){
		return 0;
	}

	int data_off = ipv4_hdr_size+tcp_hdr_size;
	if(size <= data_off+3){
		return 0;
	}
	//printf("hihi\n");
	if(my_strcmp_eq(buf+data_off, "GET", 3)){
		return 0;
	}

	// Using KMP Algorithm
	char host[7] = "Host: ";
	int h_len = strlen(host);

	int fail[6] = { 0 };
	for(int i=1,j=0;i<h_len;i++){
		while(j>0&&host[i]!=host[j])	j = fail[j-1];
		if(host[i] == host[j])	fail[i] = ++j;
	}

	for(int off=data_off+3,j=0;off<size;off++){
		while(j>0 && buf[off] != host[j])	j = fail[j-1];
		if(buf[off] == host[j]){
			if(j==h_len-1){
				off++;
				int start = off;
				while(buf[off] != '\x0d'){
					off++;
					if(off>=size){
						return 0;
					}
				}
				int site_len = off-start;
				if(should_block(buf, start, site_len)){
					return 1;
				}
			}
			else	j++;
		}
	}

	return 0;
}

struct ret_data{
	u_int32_t id;
	int block;
};

/* returns packet id */
static struct ret_data print_pkt (struct nfq_data *tb)
{	
	int id = 0;
	int block;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	u_int32_t mark,ifi;
	int ret;
	unsigned char *data;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);
	}

	mark = nfq_get_nfmark(tb);
	ifi = nfq_get_indev(tb);
	ifi = nfq_get_outdev(tb);
	ifi = nfq_get_physindev(tb);
	ifi = nfq_get_physoutdev(tb);
	ret = nfq_get_payload(tb, &data);
	if (ret >= 0){
        block = is_block(data, ret);
    }

	struct ret_data rd;
	rd.id = id;
	rd.block = block;

	return rd;
}



static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *data)
{
	struct ret_data rd = print_pkt(nfa);
	//printf("entering callback\n");
	if(rd.block){
		return nfq_set_verdict(qh, rd.id, NF_DROP, 0, NULL);
	}
	else{
		return nfq_set_verdict(qh, rd.id, NF_ACCEPT, 0, NULL);
	}
}

void usage(void){
	puts("syntax : 1m-block <site list file>");
	puts("sample : 1m-block top-1m.csv");
}

int main(int argc, char **argv)
{
	if(argc != 2){
		usage();
		exit(1);
	}

	FILE *fp = fopen(argv[1], "r");
	for(int i=0;;i++){
		if(fgets(block_site[i].url, sizeof(block_site[i].url), fp)==NULL){
			break;
		}
		block_site[i].len = strlen(block_site[i].url)-2;
		total_site++;
	}

	printf("Netfilter will Block %d sites.\n", total_site);
	
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	struct nfnl_handle *nh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));

	//printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	//printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	//printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	//printf("binding this socket to queue '0'\n");
	qh = nfq_create_queue(h,  0, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	//printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			//printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}

		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE

	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	exit(0);
	
}
