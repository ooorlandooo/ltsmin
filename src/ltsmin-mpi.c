#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <mpi.h>
#include "dlts.h"
#include "lts.h"
#include "runtime.h"
#include "scctimer.h"
#include "set.h"
#include "stream.h"
#include "mpi_core.h"
#include "mpi_io_stream.h"
#include "mpi_ram_raf.h"

#define SYNCH_BUFFER_SIZE 8000

static int branching=0;
static int synch_receive_buffer[SYNCH_BUFFER_SIZE];
static int synch_request;
static int synch_answer;
static int synch_next;
static int synch_trans;
static int synch_size=0;
static int* synch_set=NULL;
static int* synch_id=NULL;
static int synch_pending;
static uint32_t tau;


#define STRONG_REDUCTION_SET 1
#define BRANCHING_REDUCTION_SET 2

static int action=STRONG_REDUCTION_SET;
static int plain=0;

static int select_branching_reduction(char* opt,char*optarg,void *arg){
	(void)opt;(void)arg;
	branching=1;
	if(optarg==NULL){
		action=BRANCHING_REDUCTION_SET;
		return OPT_OK;
	}
	if (!strcmp(optarg,"set")) {
		action=BRANCHING_REDUCTION_SET;
		return OPT_OK;
	}
	Warning(info,"unknown branching bisimulation method: %s",optarg);
	return OPT_USAGE;
}

static int select_strong_reduction(char* opt,char*optarg,void *arg){
	(void)opt;(void)arg;
	branching=0;
	if(optarg==NULL){
		action=STRONG_REDUCTION_SET;
		return OPT_OK;
	}
	if (!strcmp(optarg,"set")) {
		action=STRONG_REDUCTION_SET;
		return OPT_OK;
	}
	Warning(info,"unknown strong bisimulation method: %s",optarg);
	return OPT_USAGE;
}

struct option options[]={
	{"",OPT_NORMAL,NULL,NULL,NULL,	"usage: ltsmin-mpi options input output",
		"This tool reduces a labeled transition system modulo bisimulation.",
		"The default bisimulation is strong bisimulation.",NULL},
	{"",OPT_NORMAL,NULL,NULL,NULL,
		"Both input and output can be a GCF archive or a set of files.",
		"To use a set of files the argument needs and occurrence of %s.",
		"If no %s is present then a GCF archive is assumed.",NULL},
	{"-plain",OPT_NORMAL,set_int,&plain,NULL,
		"Disable compression of the output.",
		"The tool detects if the input is compressed or not.",NULL,NULL},
	{"-s",OPT_NORMAL,select_strong_reduction,NULL,"-s",
		"apply strong bisimulation reduction",NULL,NULL,NULL},
	{"-b",OPT_NORMAL,select_branching_reduction,NULL,"-b",
		"apply branching bisimulation reduction",NULL,NULL,NULL},
	{"-q",OPT_NORMAL,log_suppress,&info,"-q",
		"do not print info messages",NULL,NULL,NULL},
	{"-help",OPT_NORMAL,usage,NULL,NULL,
		"print this help message",NULL,NULL,NULL},
/* for future use
	{"-S",OPT_REQ_ARG,select_strong_reduction,NULL,"-S method",
		"apply alternative strong bisimulation reduction method",
		"method is set",NULL,NULL},
	{"-B",OPT_REQ_ARG,select_branching_reduction,NULL,"-B method",
		"apply alternative branching bisimulation reduction method",
		"method is set",NULL,NULL},
*/
	{0,0,0,0,0,0,0,0,0}
};


static void synch_request_service(void *arg,MPI_Status*probe_stat){
	(void)arg;(void)probe_stat;

	MPI_Status status,*recv_status=&status;
	int len;
	int item;
	int item_len;
	int offset;
	int count;
	int set;
	int id;
	int i;

	MPI_Recv(synch_receive_buffer,SYNCH_BUFFER_SIZE,MPI_INT,MPI_ANY_SOURCE,synch_request,MPI_COMM_WORLD,recv_status);
	MPI_Get_count(recv_status,MPI_INT,&len);
	item=0;
	for(offset=0;offset<len;offset+=item_len){
		item_len=synch_receive_buffer[offset];
		count=(item_len-3)>>1;
		set=EMPTY_SET;
		for(i=0;i<count;i++){
			set=SetInsert(set,synch_receive_buffer[offset+3+2*i],synch_receive_buffer[offset+4+2*i]);
		}
//		for(i=count-1;i>=0;i--){
//			set=SetInsert(set,synch_receive_buffer[offset+3+2*i],synch_receive_buffer[offset+4+2*i]);
//		}
		id=SetGetTag(set);
		if (id<=0) {
			synch_trans+=count;
			if(synch_next==synch_size){
				synch_size+=128+(synch_size>>1);
				synch_set=(int*)realloc(synch_set,synch_size*sizeof(int));
				synch_id=(int*)realloc(synch_id,synch_size*sizeof(int));
			}
			synch_set[synch_next]=set;
			synch_id[synch_next]=synch_receive_buffer[offset+2];
			synch_next++;
			id=synch_next*mpi_nodes+mpi_me;
			SetSetTag(set,id);
		}
		synch_receive_buffer[2*item]=synch_receive_buffer[offset+1];
		synch_receive_buffer[2*item+1]=id;
		item++;
	}
	MPI_Send(synch_receive_buffer,item*2,MPI_INT,recv_status->MPI_SOURCE,synch_answer,MPI_COMM_WORLD);
}

static void synch_answer_service(void *arg,MPI_Status*probe_stat){
	(void)arg;(void)probe_stat;
	MPI_Status status,*recv_status=&status;
	int len,i;

	MPI_Recv(synch_receive_buffer,SYNCH_BUFFER_SIZE,MPI_INT,MPI_ANY_SOURCE,synch_answer,MPI_COMM_WORLD,recv_status);
	MPI_Get_count(recv_status,MPI_INT,&len);
	//Warning(1,"%d: got synch answer with %d entries",mpi_me,len>>1);
	for(i=0;i<len;i+=2){
		SetSetTag(synch_receive_buffer[i],synch_receive_buffer[i+1]);
		synch_pending--;
	}
}
/************* output stage  **********************/
TERM_STRUCT ts;
static TERM_STRUCT *term=&ts;

#define WRITE_BUFFER_SIZE 1024
static int write_tag;
static stream_t *output_src=NULL;
static stream_t *output_label=NULL;
static stream_t *output_dest=NULL;
static int scount=0;
static int *tcount=NULL;
static int write_buffer[WRITE_BUFFER_SIZE];

static void write_service(void *arg,MPI_Status*probe_stat){
	(void)arg;(void)probe_stat;
	MPI_Status status,*recv_status=&status;
	int len,i;

	MPI_Recv(write_buffer,WRITE_BUFFER_SIZE,MPI_INT,MPI_ANY_SOURCE,write_tag,MPI_COMM_WORLD,recv_status);
	MPI_Get_count(recv_status,MPI_INT,&len);
	//Warning(1,"%d: got synch answer with %d entries",mpi_me,len>>1);
	// message format is groups of 4: src_seg,src_ofs,label,dest_ofs (dest_seg==mpi_me)
	for(i=0;i<len;i+=4){
		int seg=write_buffer[i];
		DSwriteU32(output_src[seg],write_buffer[i+1]);
		DSwriteU32(output_label[seg],write_buffer[i+2]);
		DSwriteU32(output_dest[seg],write_buffer[i+3]);
		if(write_buffer[i+3]>=scount) scount=write_buffer[i+3]+1;
		tcount[seg]++;
	}
	RECV(term);
}

/******************* branching ************************/

/** FWD_BUFFER_SIZE must be multiple of 6 **/
#define FWD_BUFFER_SIZE 2400

static lts_t inv_lts;
static int inv_register;
static int fwd_tag;
static int *set,*new_set,*send_set;
static int new_count,*new_list;

static void fwd_service(void*arg,MPI_Status*probe_stat){
	(void)arg;(void)probe_stat;
	MPI_Status status,*recv_status=&status;
	int msg[FWD_BUFFER_SIZE];
	int i;
	int len;
	int s1,s2;

	MPI_Recv(msg,FWD_BUFFER_SIZE,MPI_INT,MPI_ANY_SOURCE,fwd_tag,MPI_COMM_WORLD,recv_status);
	MPI_Get_count(recv_status,MPI_INT,&len);
	for(i=0;i<len;i+=3){
		s1=set[msg[i]];
		s2=SetInsert(s1,msg[i+1],msg[i+2]);
		if(s1!=s2){
			set[msg[i]]=s2;
			if(new_set[msg[i]]==EMPTY_SET){
				new_list[new_count]=msg[i];
				new_count++;
			}
			new_set[msg[i]]=SetInsert(new_set[msg[i]],msg[i+1],msg[i+2]);
		}
	}
}

static void inv_register_service(void*arg,MPI_Status*probe_stat){
	(void)arg;(void)probe_stat;
	MPI_Status status,*recv_status=&status;
	int msg[FWD_BUFFER_SIZE];
	int i;
	int len,ofs;

	MPI_Recv(msg,FWD_BUFFER_SIZE,MPI_INT,MPI_ANY_SOURCE,inv_register,MPI_COMM_WORLD,recv_status);
	MPI_Get_count(recv_status,MPI_INT,&len);
	len=len>>1;
	//Warning(info,"%d: registering %d forwarding states",mpi_me,len);
	if (inv_lts->type!=LTS_LIST) Fatal(1,error,"%d: wrong lts type",mpi_me);
	ofs=inv_lts->transitions;
	inv_lts->transitions=ofs+len;
	for(i=0;i<len;i++){
		inv_lts->src[ofs+i]=msg[i+i];
		inv_lts->label[ofs+i]=status.MPI_SOURCE;
		inv_lts->dest[ofs+i]=msg[i+i+1];
	}
}


int main(int argc,char **argv){
	dlts_t lts;
	int **map,**newmap,iter,oldcount,newcount,localcount,*oldid,transitions,*oldsynch,*offset,root;
	int *synch_send_offset;
	int **synch_send_buffer;
	MPI_Status *status_array;
	MPI_Request *request_array;
	mytimer_t timer,compute_timer,synch_timer,exchange_timer;
	lts_t auxlts;
	int auxcount;
	int *auxmap;


	int in_count;
	int not_done;
	int *fwd_send_offset=NULL,**fwd_send_buffer=NULL;
	int total_new_set_count;
	uint32_t fwd_todo;
	int *fwd_todo_list=NULL;
	int *tmp;



        MPI_Init(&argc, &argv);

/*
	memstat_enable=1;
*/
	timer=SCCcreateTimer();
	compute_timer=SCCcreateTimer();
	synch_timer=SCCcreateTimer();
	exchange_timer=SCCcreateTimer();
/*
	verbosity=1;
*/
	RTinit(argc,&argv);
	core_init();
	set_label("bsim2mpi(%2d)",mpi_me);
	if(mpi_me!=0) core_barrier();
	take_vars(&argc,argv);
	take_options(options,&argc,argv);
	if (argc!=3) {
		Warning(info,"wrong number of options %d",argc);
		printoptions(options);
		MPI_Abort(MPI_COMM_WORLD,1);
	}
	if (mpi_me==0) switch(action){
	case STRONG_REDUCTION_SET:
		Warning(info,"reduction modulp strong bisimulation");
		break;
	case BRANCHING_REDUCTION_SET:
		Warning(info,"reduction modulo branching bisimulation");
		break;
	default:
		printoptions(options);
		MPI_Abort(MPI_COMM_WORLD,1);
	}
	if(mpi_me==0) core_barrier();
	synch_request=core_add(NULL,synch_request_service);
	synch_answer=core_add(NULL,synch_answer_service);
	if(branching){
		inv_register=core_add(NULL,inv_register_service);
		fwd_tag=core_add(NULL,fwd_service);
	}
	core_barrier();
	if (mpi_me==0) SCCstartTimer(timer);
	core_barrier();
	lts=dlts_create();
	if (strstr(argv[1],"%s")) {
		lts->arch=arch_fmt(argv[1],mpi_io_read,mpi_io_write,prop_get_U32("bs",65536));
	} else {
		if (prop_get_U32("load",1)) {
			//Warning(info,"loading");
			raf_t raf=MPI_Load_raf(argv[1],MPI_COMM_WORLD);
			core_barrier();
			lts->arch=arch_gcf_read(raf);
			core_barrier();
			//Warning(info,"archive opened");
		} else {
			lts->arch=arch_gcf_read(MPI_Create_raf(argv[1],MPI_COMM_WORLD));
		}
	}
	//Warning(info,"got the archive");
	dlts_getinfo(lts);
	core_barrier();
	//Warning(info,"got info");
	tau=lts->tau;
	if (mpi_nodes!=lts->segment_count){
		if (mpi_me==0) Warning(info,"segment count does not equal worker count");
		core_barrier();
		MPI_Finalize();
		return 0;
	}
	dlts_getTermDB(lts);
	core_barrier();
	//Warning(info,"got TermDB");
	if (mpi_me==0 && branching) Warning(info,"invisible label is %s",lts->label_string[tau]);
	status_array=(MPI_Status*)RTmalloc(2*lts->segment_count*sizeof(MPI_Status));
	request_array=(MPI_Request*)RTmalloc(2*lts->segment_count*sizeof(MPI_Request));
	offset=(int*)RTmalloc(lts->segment_count*sizeof(int));
	oldsynch=(int*)RTmalloc(lts->segment_count*sizeof(int));
	if(branching){
		fwd_todo_list=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
		new_list=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
		fwd_send_offset=(int*)RTmalloc(lts->segment_count*sizeof(int));
		fwd_send_buffer=(int**)RTmalloc(lts->segment_count*sizeof(int*));
		send_set=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
		new_set=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
	}
	map=(int**)RTmalloc(lts->segment_count*sizeof(int*));
	synch_send_offset=(int*)RTmalloc(lts->segment_count*sizeof(int));
	synch_send_buffer=(int**)RTmalloc(lts->segment_count*sizeof(int*));
	newmap=(int**)RTmalloc(lts->segment_count*sizeof(int*));
	set=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
	oldid=(int*)RTmalloc(lts->state_count[mpi_me]*sizeof(int));
	auxcount=0;
	for(int i=0;i<lts->segment_count;i++) auxcount+=lts->transition_count[mpi_me][i];
	auxmap=(int*)malloc(auxcount*sizeof(int));
	auxcount=0;
	in_count=0;
	for(int i=0;i<lts->segment_count;i++){
		//map[i]=(int*)malloc(lts->transition_count[mpi_me][i]*sizeof(int));
		map[i]=auxmap+auxcount;
		auxcount+=lts->transition_count[mpi_me][i];
		in_count+=lts->transition_count[i][mpi_me];
		synch_send_buffer[i]=(int*)RTmalloc(SYNCH_BUFFER_SIZE*sizeof(int));
		if (branching){
			fwd_send_buffer[i]=(int*)RTmalloc(FWD_BUFFER_SIZE*sizeof(int));
			fwd_send_offset[i]=0;
		}
		newmap[i]=(int*)malloc(lts->transition_count[i][mpi_me]*sizeof(int));
		for(uint32_t j=0;j<lts->transition_count[mpi_me][i];j++) map[i][j]=0;
		//Warning(info,"reading src");
		dlts_load_src(lts,mpi_me,i);
		//Warning(info,"finished src");
		//core_barrier();
		//Warning(info,"reading label");
		dlts_load_label(lts,mpi_me,i);
		//Warning(info,"reading dest");
		if (i!=mpi_me && branching) dlts_load_dest(lts,mpi_me,i);
		dlts_load_dest(lts,i,mpi_me);
	}
	Warning(info,"waiting for others");
	core_barrier();
	Warning(info,"done");
	arch_close(&(lts->arch));
	MPI_Barrier(MPI_COMM_WORLD);
	if (mpi_me==0) {
		SCCstopTimer(timer);
		SCCreportTimer(timer,"reading the LTS took");
		//resetTimer(timer);
		//startTimer(timer);
	}

	iter=0;
	oldcount=0;
	for(uint32_t i=0;i<lts->state_count[mpi_me];i++) oldid[i]=0;

	auxlts=lts_create();
	lts_set_type(auxlts,LTS_LIST);
	lts_set_size(auxlts,lts->state_count[mpi_me],auxcount);
	if(branching){
		inv_lts=lts_create();
		lts_set_type(inv_lts,LTS_LIST);
		lts_set_size(inv_lts,lts->state_count[mpi_me],in_count);
	}
	auxcount=0;
	for(int i=0;i<lts->segment_count;i++) for(uint32_t j=0;j<lts->transition_count[mpi_me][i];j++) {
		auxlts->src[auxcount]=lts->src[mpi_me][i][j];
		auxlts->label[auxcount]=lts->label[mpi_me][i][j];
		auxlts->dest[auxcount]=auxcount;
		auxcount++;
	}
	lts_set_type(auxlts,LTS_BLOCK);
	lts_sort(auxlts);
	lts_set_type(auxlts,LTS_BLOCK);

    if (branching) { /* branching reduction */
	for(;;){
		core_barrier();
		if (mpi_me==0) Warning(info,"computing signatures");
		iter++;
		SetClear(-1);
/** build initial signatures **/
		Warning(info,"%d: building initial signatures",mpi_me);
		for(uint32_t i=0;i<auxlts->states;i++){
			int s=EMPTY_SET;
			for(uint32_t j=auxlts->begin[i];j<auxlts->begin[i+1];j++){
				if ((auxlts->label[j]!=tau) || (oldid[i]!=auxmap[auxlts->dest[j]])) {
					s=SetInsert(s,auxlts->label[j],auxmap[auxlts->dest[j]]);
				}
			}
			set[i]=s;
		}
		core_barrier();
/** build forwarding structures **/
		Warning(info,"%d: building forwarding structures",mpi_me);
		lts_set_type(inv_lts,LTS_LIST);
		inv_lts->transitions=0;
		not_done=1;
		for(uint32_t j=0;not_done;j++){
			if((j&0x3f)==0)core_yield();
			not_done=0;
			for(int i=0;i<lts->segment_count;i++){
				if(j<lts->transition_count[mpi_me][i]){
					not_done=1;
					if ((lts->label[mpi_me][i][j]==tau) && (oldid[lts->src[mpi_me][i][j]]==map[i][j])) {
						fwd_send_buffer[i][fwd_send_offset[i]]=lts->src[mpi_me][i][j];
						fwd_send_buffer[i][fwd_send_offset[i]+1]=lts->dest[mpi_me][i][j];
						fwd_send_offset[i]+=2;
						if(fwd_send_offset[i]==FWD_BUFFER_SIZE){
							//Warning(info,"send");
							core_yield();
							MPI_Send(fwd_send_buffer[i],FWD_BUFFER_SIZE,MPI_INT,
									i,inv_register,MPI_COMM_WORLD);
							fwd_send_offset[i]=0;
							//Warning(info,"send OK");
						}
					}
				}
			}
		}
		for(int i=0;i<lts->segment_count;i++){
			if (fwd_send_offset[i]!=0) {
				core_yield();
				MPI_Send(fwd_send_buffer[i],fwd_send_offset[i],MPI_INT,i,inv_register,MPI_COMM_WORLD);
				fwd_send_offset[i]=0;
			}
		}
		Warning(info,"%d: submission finished",mpi_me);
		core_barrier();
		lts_set_size(inv_lts,inv_lts->states,inv_lts->transitions);
		lts_set_type(inv_lts,LTS_BLOCK_INV);
		core_barrier();
		Warning(info,"%d: got %d inbound invisible tau's",mpi_me,inv_lts->transitions);
/** forwarding along invisible tau's **/
		fwd_todo=0;
		for(uint32_t i=0;i<lts->state_count[mpi_me];i++) {
			new_set[i]=EMPTY_SET;
			send_set[i]=set[i];
			if (set[i]!=EMPTY_SET){
				fwd_todo_list[fwd_todo]=i;
				fwd_todo++;
			}
		}
		total_new_set_count=1;
		while(total_new_set_count>0){
			new_count=0;
			for(uint32_t i=0;i<fwd_todo;i++){
				int s=fwd_todo_list[i];
				if ((i&0xff)==0) core_yield();
				while(send_set[s]!=EMPTY_SET){
					int l=SetGetLabel(send_set[s]);
					int d=SetGetDest(send_set[s]);
					send_set[s]=SetGetParent(send_set[s]);
					for(uint32_t j=inv_lts->begin[s];j<inv_lts->begin[s+1];j++){
						int to=inv_lts->label[j];
						fwd_send_buffer[to][fwd_send_offset[to]]=inv_lts->src[j];
						fwd_send_buffer[to][fwd_send_offset[to]+1]=l;
						fwd_send_buffer[to][fwd_send_offset[to]+2]=d;
						fwd_send_offset[to]+=3;
						if(fwd_send_offset[to]==FWD_BUFFER_SIZE){
							core_yield();
							MPI_Send(fwd_send_buffer[to],FWD_BUFFER_SIZE,
								MPI_INT,to,fwd_tag,MPI_COMM_WORLD);
							fwd_send_offset[to]=0;
						}
					}
				}
			}
			for(int i=0;i<lts->segment_count;i++){
				if (fwd_send_offset[i]!=0) {
					core_yield();
					MPI_Send(fwd_send_buffer[i],fwd_send_offset[i],MPI_INT,i,fwd_tag,MPI_COMM_WORLD);
					fwd_send_offset[i]=0;
				}
			}
			core_barrier();
			fwd_todo=new_count;
			new_count=0;
			tmp=new_list;
			new_list=fwd_todo_list;
			fwd_todo_list=tmp;
			tmp=send_set;
			send_set=new_set;
			new_set=tmp;
			MPI_Allreduce(&fwd_todo,&total_new_set_count,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
			if (mpi_me==0) Warning(info,"sub iteration yielded %d modified sigs.",total_new_set_count);
		}
/** finished computing signatures **/
		core_barrier();
		if (mpi_me==0) Warning(info,"exchanging signatures");
		//Warning(1,"%d: exchanging signatures",mpi_me);
		localcount=0;
		synch_pending=0;
		synch_next=0;
		synch_trans=0;
		for(int i=0;i<lts->segment_count;i++) synch_send_offset[i]=0;
		for(uint32_t i=0;i<lts->state_count[mpi_me];i++){
			if(SetGetTag(set[i])<0){
				int len;
				int hash;
				int ofs;

				//if (mpi_me==0) Warning(1,"%d: new sig: %d",mpi_me,set[i]);
				//if(mpi_me==0) {
				//	fprintf(stderr,"new set: ");
				//	SetPrint(stderr,set[i]);
				//	fprintf(stderr,"\n");
				//}
				localcount++;
				SetSetTag(set[i],0);
				hash=SetGetHash(set[i])%mpi_nodes;
				len=2*SetGetSize(set[i])+3;
				if((synch_send_offset[hash]+len)>SYNCH_BUFFER_SIZE) { // buffer requests
				//if(synch_send_buffer[hash]){ // send requests immediately
					//Warning(1,"%d: sending full buffer",mpi_me);
					core_yield();
					MPI_Send(synch_send_buffer[hash],synch_send_offset[hash],MPI_INT,
								hash,synch_request,MPI_COMM_WORLD);
					synch_send_offset[hash]=0;
				}
				if(len>SYNCH_BUFFER_SIZE) Fatal(1,error,"SYNCH_BUFFER_SIZE too small");
				//Warning(1,"%d: adding new sig to buffer",mpi_me);
				ofs=synch_send_offset[hash];
				synch_send_offset[hash]+=len;
				synch_send_buffer[hash][ofs]=len;
				synch_send_buffer[hash][ofs+1]=set[i];
				synch_send_buffer[hash][ofs+2]=oldid[i];
				SetGetSet(set[i],(synch_send_buffer[hash])+ofs+3);
				synch_pending++;
				//core_yield();
			}
			if ((i&0x3fff)==0) core_yield();
		}
		//Warning(1,"%d: sending partial buffers",mpi_me);
		for(int i=0;i<lts->segment_count;i++) {
			if (synch_send_offset[i]>0){
				core_yield();
				MPI_Send(synch_send_buffer[i],synch_send_offset[i],MPI_INT,i,synch_request,MPI_COMM_WORLD);
			}
		}
		Warning(info,"%d: submitted all requests",mpi_me);
		while(synch_pending) core_wait(synch_answer);
		Warning(info,"%d: got all sigs",mpi_me);
		core_barrier();
		for(int i=0;i<lts->segment_count;i++){
			int jmax=lts->transition_count[i][mpi_me];
			int *nm=newmap[i];
			uint32_t *s=lts->dest[i][mpi_me];
			for(int j=0;j<jmax;j++){
				nm[j]=SetGetTag(set[s[j]]);
			}
		}
		Warning(info,"%d: share is %d",mpi_me,synch_next);
		MPI_Barrier(MPI_COMM_WORLD);
		MPI_Allreduce(&synch_next,&newcount,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
		if (mpi_me==0) Warning(info,"count of iteration %d is %d",iter,newcount);
		if (oldcount==newcount) break;
		for(uint32_t i=0;i<lts->state_count[mpi_me];i++){
			oldid[i]=SetGetTag(set[i]);
		}
		MPI_Alltoallv(&synch_next,mpi_ones,mpi_zeros,MPI_INT,oldsynch,mpi_ones,mpi_indices,MPI_INT,MPI_COMM_WORLD);
		oldcount=newcount;
		if (mpi_me==0) Warning(info,"updating map");
		for(int i=0;i<lts->segment_count;i++){
			MPI_Isend(newmap[i],lts->transition_count[i][mpi_me],MPI_INT,i,37,MPI_COMM_WORLD,request_array+i);
			MPI_Irecv(map[i],lts->transition_count[mpi_me][i],MPI_INT,i,37,MPI_COMM_WORLD,request_array+lts->segment_count+i);
		}
		MPI_Waitall(2*lts->segment_count,request_array,status_array);
		MPI_Barrier(MPI_COMM_WORLD);
	}
    } else { /* strong reduction */
	for(;;){
		core_barrier();
//		startTimer(compute_timer);
		if (mpi_me==0) Warning(info,"computing signatures");
		iter++;
//		MEMSTAT_CHECK;
		SetClear(-1);
/***************
		for(i=0;i<lts->state_count[mpi_me];i++) set[i]=EMPTY_SET;
		for(i=0;i<lts->segment_count;i++){
			int *s=lts->src[mpi_me][i];
			int *l=lts->label[mpi_me][i];
			int *m=map[i];
			int jmax=lts->transition_count[mpi_me][i];
			for(j=0;j<jmax;j++){
				int *ptr=&(set[s[j]]);
				*ptr=SetInsert(*ptr,l[j],m[j]);
			}
		}
***************/
		for(uint32_t i=0;i<auxlts->states;i++){
			int s=EMPTY_SET;
			for(uint32_t j=auxlts->begin[i];j<auxlts->begin[i+1];j++){
				s=SetInsert(s,auxlts->label[j],auxmap[auxlts->dest[j]]);
			}
			set[i]=s;
		}
//		stopTimer(compute_timer);
		core_barrier();
//		startTimer(synch_timer);
		//if (mpi_me==0)
		Warning(info,"%d: exchanging signatures",mpi_me);
		localcount=0;
		synch_pending=0;
		synch_next=0;
		synch_trans=0;
		for(int i=0;i<lts->segment_count;i++) synch_send_offset[i]=0;
		for(uint32_t i=0;i<lts->state_count[mpi_me];i++){
			if(SetGetTag(set[i])<0){
				int len;
				int hash;
				int ofs;

				//if (mpi_me==0) Warning(1,"%d: new sig: %d",mpi_me,set[i]);
				//if(mpi_me==0) {
				//	fprintf(stderr,"new set: ");
				//	SetPrint(stderr,set[i]);
				//	fprintf(stderr,"\n");
				//}
				localcount++;
				SetSetTag(set[i],0);
				hash=SetGetHash(set[i])%mpi_nodes;
				len=2*SetGetSize(set[i])+3;
				if((synch_send_offset[hash]+len)>SYNCH_BUFFER_SIZE) { // buffer requests
				//if(synch_send_buffer[hash]){ // send requests immediately
					//Warning(1,"%d: sending full buffer",mpi_me);
					core_yield();
					MPI_Send(synch_send_buffer[hash],synch_send_offset[hash], 							MPI_INT,hash,synch_request,MPI_COMM_WORLD);
					synch_send_offset[hash]=0;
				}
				if(len>SYNCH_BUFFER_SIZE) Fatal(1,error,"SYNCH_BUFFER_SIZE too small");
				//Warning(1,"%d: adding new sig to buffer",mpi_me);
				ofs=synch_send_offset[hash];
				synch_send_offset[hash]+=len;
				synch_send_buffer[hash][ofs]=len;
				synch_send_buffer[hash][ofs+1]=set[i];
				synch_send_buffer[hash][ofs+2]=oldid[i];
				SetGetSet(set[i],(synch_send_buffer[hash])+ofs+3);
				synch_pending++;
				//core_yield();
			}
			if ((i&0x3fff)==0) core_yield();
		}
		//Warning(1,"%d: sending partial buffers",mpi_me);
		for(int i=0;i<lts->segment_count;i++) {
			if (synch_send_offset[i]>0){
				core_yield();
				MPI_Send(synch_send_buffer[i],synch_send_offset[i],MPI_INT,i,synch_request,MPI_COMM_WORLD);
			}
		}
		Warning(info,"%d: submitted all requests",mpi_me);
		while(synch_pending) core_wait(synch_answer);
		Warning(info,"%d: got all sigs",mpi_me);
		core_barrier();
		for(int i=0;i<lts->segment_count;i++){
			int jmax=lts->transition_count[i][mpi_me];
			int *nm=newmap[i];
			uint32_t *s=lts->dest[i][mpi_me];
			for(int j=0;j<jmax;j++){
				nm[j]=SetGetTag(set[s[j]]);
			}
		}
		Warning(info,"%d: share is %d",mpi_me,synch_next);
		MPI_Barrier(MPI_COMM_WORLD);
		MPI_Allreduce(&synch_next,&newcount,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
		if (mpi_me==0) Warning(info,"count of iteration %d is %d",iter,newcount);
//		stopTimer(synch_timer);
		if (oldcount==newcount) break;
//		startTimer(exchange_timer);
		for(uint32_t i=0;i<lts->state_count[mpi_me];i++){
			oldid[i]=SetGetTag(set[i]);
		}
		MPI_Alltoallv(&synch_next,mpi_ones,mpi_zeros,MPI_INT,oldsynch,mpi_ones,mpi_indices,MPI_INT,MPI_COMM_WORLD);
		oldcount=newcount;
		if (mpi_me==0) Warning(info,"updating map");
		for(int i=0;i<lts->segment_count;i++){
			MPI_Isend(newmap[i],lts->transition_count[i][mpi_me],MPI_INT,i,37,MPI_COMM_WORLD,request_array+i);
			MPI_Irecv(map[i],lts->transition_count[mpi_me][i],MPI_INT,i,37,MPI_COMM_WORLD,
							request_array+lts->segment_count+i);
		}
//		MEMSTAT_CHECK;
		MPI_Waitall(2*lts->segment_count,request_array,status_array);
//		stopTimer(exchange_timer);
		MPI_Barrier(MPI_COMM_WORLD);
	}
    } /* end of strong reduction */


#define GET_SEG(id) (id%mpi_nodes)
#define GET_OFS(id) (id/mpi_nodes-1)


	MPI_Reduce(&synch_trans,&transitions,1,MPI_INT,MPI_SUM,0,MPI_COMM_WORLD);
	if ((uint32_t)mpi_me==lts->root_seg) {
		Warning(info,"reduced state space has %d states and %d transitions",newcount,transitions);
		root=oldid[lts->root_ofs];
		Warning(info,"root is %d/%d",GET_SEG(root),GET_OFS(root));
	}
	MPI_Bcast(&root,1,MPI_INT,lts->root_seg,MPI_COMM_WORLD);
	Warning(info,"%d: root is %d/%d",mpi_me,GET_SEG(root),GET_OFS(root));
	core_barrier();

	archive_t arch;
	if (strstr(argv[2],"%s")){
		arch=arch_fmt(argv[2],mpi_io_read,mpi_io_write,prop_get_U32("bs",65536));
	} else {
		uint32_t bs=prop_get_U32("bs",65536);
		uint32_t bc=prop_get_U32("bc",128);
		arch=arch_gcf_create(MPI_Create_raf(argv[2],MPI_COMM_WORLD),bs,bs*bc,mpi_me,mpi_nodes);
	}
	
	output_src=(stream_t*)RTmalloc(mpi_nodes*sizeof(FILE*));
	output_label=(stream_t*)RTmalloc(mpi_nodes*sizeof(FILE*));
	output_dest=(stream_t*)RTmalloc(mpi_nodes*sizeof(FILE*));
	tcount=(int*)RTmalloc(mpi_nodes*sizeof(int));
	for(int i=0;i<mpi_nodes;i++){
		char name[1024];
		sprintf(name,"src-%d-%d",i,mpi_me);
		output_src[i]=arch_write(arch,name,plain?NULL:"diff32|gzip");
		sprintf(name,"label-%d-%d",i,mpi_me);
		output_label[i]=arch_write(arch,name,plain?NULL:"gzip");
		sprintf(name,"dest-%d-%d",i,mpi_me);
		output_dest[i]=arch_write(arch,name,plain?NULL:"diff32|gzip");
		tcount[i]=0;
	}
	write_tag=core_add(NULL,write_service);
	TERM_INIT(term);
	MPI_Barrier(MPI_COMM_WORLD);
	Warning(info,"%d: starting to write using tag %d",mpi_me,write_tag);
	for(int j=0;j<synch_next;j++){
		int set;
		int src;
		int label;
		int dest;
		src=synch_id[j];
		for(set=synch_set[j];set!=EMPTY_SET;set=SetGetParent(set)){
			label=SetGetLabel(set);
			//char*lbl=lts->label_string[label];
			dest=SetGetDest(set);
			int msg[4];
			msg[0]=GET_SEG(src);
			msg[1]=GET_OFS(src);
			msg[2]=label;
			msg[3]=GET_OFS(dest);
			MPI_Send(msg,4,MPI_INT,GET_SEG(dest),write_tag,MPI_COMM_WORLD);
			SEND(term);
		}
		core_yield();
	}
	Warning(info,"%d: waiting for write to finish",mpi_me);
	core_terminate(term);
	Warning(info,"%d: closing files",mpi_me);
	for(int i=0;i<mpi_nodes;i++){
		DSclose(&output_src[i]);
		DSclose(&output_label[i]);
		DSclose(&output_dest[i]);
	}
	stream_t infos;
	int *temp=NULL;
	if (mpi_me==0){
		stream_t ds=arch_write(arch,"TermDB",plain?NULL:"gzip");
		for(int i=0;i<lts->label_count;i++){
			char*ln=lts->label_string[i];
			DSwrite(ds,ln,strlen(ln));
			DSwrite(ds,"\n",1);
		}
		DSclose(&ds);
		infos=arch_write(arch,"info",plain?NULL:"");
		DSwriteU32(infos,31);
		DSwriteS(infos,"generated by mpi_min");
		DSwriteU32(infos,mpi_nodes);
		DSwriteU32(infos,GET_SEG(root));
		DSwriteU32(infos,GET_OFS(root));
		DSwriteU32(infos,lts->label_count);
		DSwriteU32(infos,tau);
		DSwriteU32(infos,0);
		temp=(int*)malloc(mpi_nodes*mpi_nodes*sizeof(int));
	}
	MPI_Gather(&scount,1,MPI_INT,temp,1,MPI_INT,0,MPI_COMM_WORLD);
	long long int total_states=0;
	if (mpi_me==0){
		for(int i=0;i<mpi_nodes;i++){
			total_states+=temp[i];
			DSwriteU32(infos,temp[i]);
		}
	}
	MPI_Gather(tcount,mpi_nodes,MPI_INT,temp,mpi_nodes,MPI_INT,0,MPI_COMM_WORLD);
	long long int total_transitions=0;
	if (mpi_me==0){
		for(int i=0;i<mpi_nodes;i++){
			for(int j=0;j<mpi_nodes;j++){
				total_transitions+=temp[i+mpi_nodes*j];
				//ATwarning("%d -> %d : %d",i,j,temp[i+mpi_nodes*j]);
				DSwriteU32(infos,temp[i+mpi_nodes*j]);
			}
		}
		DSclose(&infos);
		Warning(info,"wrote %lld states and %lld transitions",total_states,total_transitions);
	}
	arch_close(&arch);

	core_barrier();
	MPI_Finalize();
	Warning(info,"That's all!");
	return 0;
}

