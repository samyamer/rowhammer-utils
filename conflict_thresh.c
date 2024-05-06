#include "rowhammer_utils.h"
#define _GNU_SOURCE
#include <sys/mman.h>
#define ROW_CONFLICT_TH 450



// #include "allocator.h"
// #include "include/params.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

// #include "utils.h"
#define POSIX_ALIGN (1<<22)
#define MEM_SIZE (1<<21)
#define HUGE_SIZE (1<<21)

#define HASH_FN_CNT 4
#define ROW_SIZE 		(1<<13)

typedef struct {
	uint64_t lst[HASH_FN_CNT];
	uint64_t len;
}AddrFns;

typedef struct {
	AddrFns h_fns;
	uint64_t row_mask;
	uint64_t col_mask;
}DRAMLayout;


typedef struct{
    uint64_t num_conf;
    char** conflicts;
}contig_rows;

typedef struct {
   char* base;
   contig_rows* bank_contig_rows; // array of conflict array (e.g. conflict_bank[0] is an array of all the addresses that conflict in bank 0. Each 4 addresses in that row will belong to a single row)
   uint64_t virt_offset; // the virtual addr offset from eg. a virtual address that thas 21 trailing zeroes has a virtual offset of 0. Helps with virt2dram but not abs necessary
}contig_chunk;

typedef struct{
    char* aggressors[8];
    char* victims[4];
}ds_pair; // this struct will hold all the relevant base addresses for a double-sided hammer

typedef struct{
    ds_pair* pairs[16];
    int num_pairs[16]; 
}bank_pairs;

DRAMLayout      g_mem_layout = {{{0x2040,0x24000,0x48000,0x90000}, 4}, 0x3fffe0000, ROW_SIZE-1};

bank_pairs bank_pairs_arr;


int num_blocks = 5;
// only works with 2MB aligned pages because the first 21 bits of the virtual addresses are identical to the first in phys
u_int64_t get_bank(uint64_t v_addr){
    
    uint64_t bank=0;
    for (int i = 0; i < g_mem_layout.h_fns.len; i++) {
		
		bank |= (__builtin_parityl(v_addr & g_mem_layout.h_fns.lst[i]) <<i);
	}
    return bank;
}

///thp code 
uint64_t get_dram_row_thp(uint64_t v_addr)
{
	uint64_t row_mask = (HUGE_SIZE -1) & g_mem_layout.row_mask;
	return (v_addr & row_mask) >> __builtin_ctzl(row_mask);
}

int alloc_buffer(contig_chunk* mem_chunks, int num_blocks)
{
	// if (mem->buffer[0] != NULL) {
	// 	fprintf(stderr, "[ERROR] - Memory already allocated\n");
	// }s

	// if (mem->align < _SC_PAGE_SIZE) {
	// 	mem->align = 0;
	// }

	//madvise(mem->buffer, mem->size, MADV_HUGEPAGE);

	//uint64_t alloc_size = mem->align ? mem->size + mem->align : mem->size;
	// uint64_t alloc_size = mem->size;
	//uint64_t alloc_flags = MAP_PRIVATE | MAP_POPULATE | MAP_NORESERVE | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT) | MAP_ANONYMOUS;
	//uint64_t alloc_flags = MAP_PRIVATE | MAP_POPULATE | MAP_NORESERVE | MAP_ANONYMOUS;
	// uint64_t alloc_flags = MAP_PRIVATE | MAP_POPULATE;


	// madvise alloc
	for (int i = 0; i < num_blocks; i++) {
		posix_memalign((void **)(&(mem_chunks[i].base)), POSIX_ALIGN, MEM_SIZE);
        if (mem_chunks[i].base == NULL) {
		fprintf(stderr, "wtf\n");
	    }
		//fprintf(stderr, "iter %d: ", i);
		if (madvise(mem_chunks[i].base, POSIX_ALIGN, MADV_HUGEPAGE) == -1)
		{
			fprintf(stderr, "MADV %d Failed: %d\n", i, errno);
            if(errno == ENOMEM){
                fprintf(stderr, "no mem");
            }
		}
		// *(mem[i]) = 10;
        memset(mem_chunks[i].base,0xFF,MEM_SIZE);
		//else
		//{
		//	fprintf(stderr, "MADV Success\n");
		//}
        if(check_consecutive((uint64_t) mem_chunks[i].base,MEM_SIZE)){
            printf("cons\n");
        }else{printf("not cons \n");}
        mem_chunks[i].virt_offset = ((uint64_t) mem_chunks[i].base) & (MEM_SIZE - 1);
	}
	if (mem_chunks[0].base != NULL) {
		fprintf(stderr, "Buffer allocated\n");
	}

	
    // fprintf(stderr, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    // fprintf(stderr, "[ MEM ] - Buffer:      %p\n", mem->buffer);
    // fprintf(stderr, "[ MEM ] - Size:        %ld\n", alloc_size);
    // fprintf(stderr, "[ MEM ] - Alignment:   %ld\n", mem->align);
    // fprintf(stderr, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
	// exit(0);
	return 0;

}

// finds all pages that row conflict with the base_page
void find_bank_conflict(contig_chunk* mem_chunk, int target_bank){

    // find a base address in the target bank
    
    u_int64_t base;
    u_int64_t curr;
    uint64_t phys;
    int ndx=0;
    for(int i=0; i< MEM_SIZE/PAGE_SIZE; i++){
        int found_base=0;
        for(int j=0; j<2; j++){
            base = (uint64_t)mem_chunk->base + i*PAGE_SIZE + 64*j;
            if(get_bank(base) == target_bank){
                printf("Found base\n");
                found_base=1;
                break;
            }
        }
        if(found_base){break;}
    }
    mem_chunk->bank_contig_rows[target_bank].conflicts = (char**) malloc(sizeof(char*) * 300);
    
    

    DramAddr* dram = (DramAddr*) malloc(sizeof(DramAddr));
    phys = get_physical_addr(base);
    dram_address(phys, dram);

    printf("Base page memory\n");
   
    printf("BA0:%lx BA1:%lx BA2:%lx BA3:%lx row:%lx\n", dram->BA0,dram->BA1, dram->BA2, dram->BA3, dram->row_num);
    printf("%lx\n",phys);
    printf("----------------------------\n");
    printf("starting row conflict\n");
    for(int i=0; i<MEM_SIZE/PAGE_SIZE; i++){
        for (int v=0; v< 2; v++){
            curr = (uint64_t) mem_chunk->base + (i*PAGE_SIZE) + 64*v;
            if(curr == base){continue;}
            
            u_int64_t acc_time = 0;
            u_int64_t time;
            int j=0;
            
            while(j<8000){
                time = row_conflict_time((u_int64_t) (base), curr);
                if(time > 1000){continue;}
                j++;
                acc_time+=time;
            }
            acc_time = acc_time/8000;

            // for visibility
            if(acc_time > ROW_CONFLICT_TH){  
                phys = get_physical_addr(curr);
                dram_address(phys, dram);
                printf("%d\n", ndx);
                // printf("BA0:%lx BA1:%lx BA2:%lx BA3:%lx row:%lx\n", dram->BA0,dram->BA1, dram->BA2, dram->BA3, dram->row_num);
                printf("Get DRAM row: %lx\n",get_dram_row_thp((u_int64_t)curr));
                printf("Get BANK: %lx\n",get_bank((u_int64_t)curr));
                printf("(%lx, %lx)\n",curr,phys);
                printf("%lu\n",acc_time);
                printf("----------------------------\n");
                // add to array
                mem_chunk->bank_contig_rows[target_bank].conflicts[ndx] = (char*)curr;
                ndx++;

            }
        }

        // if(ndx >=100){break;}
        

    }
    free(dram);
    // if(ndx < 100){printf("less than 100 found\n"); exit(0);}
    mem_chunk->bank_contig_rows[target_bank].num_conf =ndx;
    return ;
}


// on stalin (ddr4) each row has data from 4 pages/
// so, in conflict array, every 4 entries correspond to same row
// In the conflict array, the first two entries are pages with data in that row from  odd blocks (1,3,5...)
// the second two entries are pages with data in even blocks (0,2,4,...)
int memory_template(char** conflicts, int num_conf){
    // victim index will step by 2 from 2 to n-4
    int victim_ndx =36;

    char** agg_list = malloc(5*sizeof(char*));

    while(victim_ndx <= (num_conf - 8) ){
        // set the aggressors

        printf("--------------------------------------------------------------------\n");

    
        
        memset(conflicts[victim_ndx -4], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx -3], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx -2], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx -1], 0xFF, PAGE_SIZE);
        
        memset(conflicts[victim_ndx+4], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx+5], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx+6], 0xFF, PAGE_SIZE);
        memset(conflicts[victim_ndx+7], 0xFF, PAGE_SIZE);

      
        agg_list[0] = conflicts[victim_ndx -4];
        agg_list[1] = conflicts[victim_ndx +4];
        agg_list[2] = conflicts[victim_ndx +8];
        // printf("Aggressors:\n");
        // for(int a=0; a< 3; a++){
        //     printf("%lx\t", get_physical_addr((u_int64_t)agg_list[a]));
        // }
        // printf("\n");

        // check for correct flips
            // flips in only the first 32 bytes
    
        int flippy;
        int bad = 0;
        int num_flips=0;
        
        
        for(int i=0; i<4;i++){
            memset(conflicts[victim_ndx +i], 0x00, PAGE_SIZE);
            flush_chunck(conflicts[victim_ndx+i],PAGE_SIZE);
            hammer(conflicts[victim_ndx -4],conflicts[victim_ndx+4] , 1000000);
            // multi_sided(agg_list, 3, 900000);
            flippy = 0;
            bad = 0;
            num_flips=0;
            

            for(int j=0; j<PAGE_SIZE; j++){
                if((uint8_t) conflicts[victim_ndx+i][j] != 0x00 ){
                    printf("FLIP in addr (%lx, %lx): %hhx\n", get_physical_addr((u_int64_t)(conflicts[victim_ndx+i] + j)),(u_int64_t)(conflicts[victim_ndx+i] + j), conflicts[victim_ndx+i][j]);
                }
            }
            
        }

       

        victim_ndx+=4;

    }
    
    return -1;
    
}

void print_pairs(ds_pair* pairs_array, int num_pairs){
    for(int i=0; i< num_pairs; i++){
        printf("(%lx, %lx)\n",(uint64_t)  pairs_array[i].aggressors[0],  get_physical_addr((u_int64_t)pairs_array[i].aggressors[0]));
    }
}
// takes in all the memory chunks and returns a pointer to a list of hammering pairs
// the list is to be used in templating
// so this function is just for organizing things
int create_hammer_pairs(contig_chunk* chunks, int target_bank, ds_pair* pairs_array){
    // ds_pair* pairs_array = (ds_pair*) malloc(200*sizeof(ds_pair));
    int pair_ndx=0;

    for(int i=0; i< num_blocks; i++){ // go through the conflict array of the target bank of each chunk. The conflict array is basically a list of contig memory rows
        if(chunks[i].bank_contig_rows[target_bank].num_conf < 12){continue;} // if the chunk doesn't have a full 3 rows for double sided, skip it
        int ndx=0;
        int agg_parity=0;
        while((ndx*4) <= chunks[i].bank_contig_rows[target_bank].num_conf  -4){
            if((ndx%2) == 0){
                // aggressors
                pairs_array[pair_ndx].aggressors[0 + 4*agg_parity] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4];
                pairs_array[pair_ndx].aggressors[1 + 4*agg_parity] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+1];
                pairs_array[pair_ndx].aggressors[2 + 4*agg_parity] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+2];
                pairs_array[pair_ndx].aggressors[3 + 4*agg_parity] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+3];

                if(agg_parity == 1){
                    // completed an agg pair
                    pair_ndx++;
                }
                agg_parity = agg_parity ^ 1;

            }else{
                pairs_array[pair_ndx].victims[0] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4];
                pairs_array[pair_ndx].victims[1] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+1];
                pairs_array[pair_ndx].victims[2] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+2];
                pairs_array[pair_ndx].victims[3] = chunks[i].bank_contig_rows[target_bank].conflicts[ndx*4+3];
            }
            ndx++;
        }
        
    }
    print_pairs(pairs_array,  pair_ndx);
    return pair_ndx;
}



void fill_victims(ds_pair* pairs_array, int num_pairs){
    for(int i=0; i< num_pairs; i++){
        char* addr =  (char*) (((uint64_t)  pairs_array[i].victims[0])  &(~(PAGE_SIZE-1)) );
        char* addr2 = (char*) (((uint64_t)  pairs_array[i].victims[1])  &(~(PAGE_SIZE-1)) );
        char* addr3 = (char*) (((uint64_t)  pairs_array[i].victims[2])  &(~(PAGE_SIZE-1)) );
        char* addr4 = (char*) (((uint64_t)  pairs_array[i].victims[3])  &(~(PAGE_SIZE-1)) );
        memset(addr, 0x00, PAGE_SIZE);
        memset(addr2, 0x00, PAGE_SIZE);
        memset(addr3, 0x00, PAGE_SIZE);
        memset(addr4, 0x00, PAGE_SIZE);
        flush_chunck(addr,PAGE_SIZE);
        flush_chunck(addr2,PAGE_SIZE);
        flush_chunck(addr3,PAGE_SIZE);
        flush_chunck(addr4,PAGE_SIZE);
    }
    return;
}

void check_victims(ds_pair* pairs_array, int num_pairs, uint8_t expected){
    for(int i=0; i< num_pairs; i++){
        for(int j=0; j < 4; j++){
            char* addr = (char*) (((uint64_t) pairs_array[i].victims[j]) &(~(PAGE_SIZE-1)) );
            // printf("checking (%lx, %lx)\n", get_physical_addr((u_int64_t)(conflicts[ndx*4 +i])),(u_int64_t)(conflicts[ndx*4 +i]));
            for(int n=0; n<PAGE_SIZE; n++){
                if((uint8_t) addr[n] != expected ){
                    printf("FLIP in addr (%lx, %lx): %hhx\n", get_physical_addr((u_int64_t)(addr + n)),(u_int64_t)(addr + n), addr[n]);
                }
            }
        }   
    }

}


// TODO: FIX BUG
// the pairs has the two aggressors in a single array
int ten_sided_temp(ds_pair* pairs_array, int num_pairs){
    if(num_pairs < 5){return 0;}
    
    // for every pair, fill the victims with data. Agg already flled at the first allocation
    
    char** agg_list = malloc(10*sizeof(char*));
    // go through the aggressor pairs in slidiing window
    int agg_window_start=0;
    int agg_window_end=1;
    agg_list[0] = pairs_array[agg_window_start].aggressors[0]; // add top and bottom agg
    agg_list[1] = pairs_array[agg_window_start].aggressors[4];
    int ndx=2;
    while(agg_window_end < num_pairs){
        agg_list[ndx] = pairs_array[agg_window_end].aggressors[0];
        agg_list[ndx+1] = pairs_array[agg_window_end].aggressors[4];
        ndx+=2;
        agg_window_end++;
        if(ndx>=10){ // got a list of 10 aggressors
            
            // fill victims and hammer
            
            printf("%d  aggressors\n", ndx);
            for(int i=0; i<ndx; i++){
                printf("%lx\n", get_physical_addr((u_int64_t) agg_list[i]));
            }
            for(int i=0; i<10; i++){
                fill_victims(pairs_array, num_pairs);
                int x =  hammer_thp_prehammer(agg_list, i, 10);
                check_victims(pairs_array, num_pairs, 0x00);
            }

            // reset
            agg_window_start++;
            agg_window_end = agg_window_start+1;
            ndx=0;
            agg_list[ndx] = pairs_array[agg_window_start].aggressors[0];
            agg_list[ndx+1] = pairs_array[agg_window_start].aggressors[4];
            ndx+=2;

        }
    }
    return 0;
    
    // while((ndx*4) <= num_conf -4){
        
    //     if((ndx%2) == 0){
    //         printf("agg ndx %d\n", ndx*4);
    //         // set the aggressors to 1
    //         // memset(  (char*) (((uint64_t) conflicts[ndx*4]) &(~(PAGE_SIZE-1)) ), 0xFF, PAGE_SIZE);
    //         // memset((char*) (((uint64_t) conflicts[ndx*4 +1]) &(~(PAGE_SIZE-1)) ), 0xFF, PAGE_SIZE);
    //         // memset((char*) (((uint64_t) conflicts[ndx*4 +2]) &(~(PAGE_SIZE-1)) ), 0xFF, PAGE_SIZE);
    //         // memset((char*) (((uint64_t) conflicts[ndx*4 +3]) &(~(PAGE_SIZE-1)) ), 0xFF, PAGE_SIZE);

    //         printf("Entering ndx %d into list \n", ndx*4);
    //         printf("(%lx, %lx) \n",  get_physical_addr((u_int64_t)( conflicts[ndx*4])),(u_int64_t)( conflicts[ndx*4]));
    //         agg_list[num_agg] = conflicts[ndx*4];
    //         num_agg++;
    //     }else{
    //         char* addr = (char*) (((uint64_t) conflicts[ndx*4 ]) &(~(PAGE_SIZE-1)) );
    //         char* addr2 = (char*) (((uint64_t) conflicts[ndx*4 +1]) &(~(PAGE_SIZE-1)) );
    //         char* addr3 = (char*) (((uint64_t) conflicts[ndx*4 +2]) &(~(PAGE_SIZE-1)) );
    //         char* addr4 = (char*) (((uint64_t) conflicts[ndx*4 +3]) &(~(PAGE_SIZE-1)) );
    //         // set the victiim to 0
    //         memset(addr, 0x00, PAGE_SIZE);
    //         memset(addr2, 0x00, PAGE_SIZE);
    //         memset(addr3, 0x00, PAGE_SIZE);
    //         memset(addr4, 0x00, PAGE_SIZE);
    //         flush_chunck(conflicts[ndx*4],PAGE_SIZE);
    //         flush_chunck(conflicts[ndx*4+1],PAGE_SIZE);
    //         flush_chunck(conflicts[ndx*4+2],PAGE_SIZE);
    //         flush_chunck(conflicts[ndx*4+3],PAGE_SIZE);

    //     }
    //     ndx++;
    // }
   
    // printf("%d - aggressors\n", num_agg);
    // for(int i=0; i<num_agg; i++){
    //     printf("%lx\n", get_physical_addr((u_int64_t) agg_list[i]));
    // }
    // // exit(0);
    // if(num_agg>10){num_agg=10;}

    // for(int i=0; i < num_agg; i++){
    //    int x =  hammer_thp_prehammer(agg_list, i, num_agg);

    //     // check for flips
    //         ndx=0;
    //         while((ndx*4) <= num_conf -4){
    //             // printf("%d --------\n", ndx);        
    //             if((ndx%2) == 0){ndx++; continue;}
                
    //             for(int i=0; i < 4; i++){
    //                 char* addr = (char*) (((uint64_t) conflicts[ndx*4 +i]) &(~(PAGE_SIZE-1)) );
    //                 // printf("checking (%lx, %lx)\n", get_physical_addr((u_int64_t)(conflicts[ndx*4 +i])),(u_int64_t)(conflicts[ndx*4 +i]));
    //                 for(int j=0; j<PAGE_SIZE; j++){
    //                     if((uint8_t) addr[j] != 0x00 ){
    //                         printf("FLIP in addr (%lx, %lx): %hhx\n", get_physical_addr((u_int64_t)(addr + j)),(u_int64_t)(addr + j), addr[j]);
    //                     }
    //                 }
    //             }
    //             memset(  (char*) (((uint64_t) conflicts[ndx*4]) &(~(PAGE_SIZE-1)) ), 0x00, PAGE_SIZE);
    //             memset((char*) (((uint64_t) conflicts[ndx*4 +1]) &(~(PAGE_SIZE-1)) ), 0x00, PAGE_SIZE);
    //             memset((char*) (((uint64_t) conflicts[ndx*4 +2]) &(~(PAGE_SIZE-1)) ), 0x00, PAGE_SIZE);
    //             memset((char*) (((uint64_t) conflicts[ndx*4 +3]) &(~(PAGE_SIZE-1)) ), 0x00, PAGE_SIZE);
    //             flush_chunck(conflicts[ndx*4],PAGE_SIZE);
    //             flush_chunck(conflicts[ndx*4+1],PAGE_SIZE);
    //             flush_chunck(conflicts[ndx*4+2],PAGE_SIZE);
    //             flush_chunck(conflicts[ndx*4+3],PAGE_SIZE);
    //             ndx++;
    //         }
    // }
    

   
}

void multi_bank(){
    char** agg_list = malloc(20*sizeof(char*));
   for(int bank1=0; bank1<16; bank1++){
       for(int bank2=(bank1+1); bank2<16; bank2++){
           int bank1_start=0;
           int bank2_start=0;
           int bank1_end=0;
           int bank2_end=0;
           int ndx=0;
           while((bank1_end < bank_pairs_arr.num_pairs[bank1]) &&  (bank2_end < bank_pairs_arr.num_pairs[bank2])){
            //    agg_list[ndx] = bank_pairs_arr.pairs[bank1][bank1_end].aggressors[0];
            //    agg_list[ndx+1] = bank_pairs_arr.pairs[bank1][bank1_end].aggressors[4];
            //    agg_list[ndx+2] = bank_pairs_arr.pairs[bank2][bank2_end].aggressors[0];
            //    agg_list[ndx+3] = bank_pairs_arr.pairs[bank2][bank2_end].aggressors[4];
               
               agg_list[ndx] = bank_pairs_arr.pairs[bank1][bank1_end].aggressors[0];
            //    agg_list[ndx+1] = bank_pairs_arr.pairs[bank1][bank1_end].aggressors[4];
                agg_list[ndx+1] = bank_pairs_arr.pairs[bank2][bank2_end].aggressors[0];
               agg_list[ndx+2] =  bank_pairs_arr.pairs[bank1][bank1_end].aggressors[4];
               agg_list[ndx+3] = bank_pairs_arr.pairs[bank2][bank2_end].aggressors[4];
               ndx+=4;
               bank1_end++;
               bank2_end++;
               if(ndx>=20){
                   printf("%d  aggressors\n", ndx);
                   for(int i=0; i<ndx; i++){
                       printf("%lx\t", get_physical_addr((u_int64_t) agg_list[i]));
                       printf("Bank %ld:\n", get_bank((u_int64_t) agg_list[i]));
                    }
                   // fill the victims
                   fill_victims(bank_pairs_arr.pairs[bank1], bank_pairs_arr.num_pairs[bank1]);
                   fill_victims(bank_pairs_arr.pairs[bank2], bank_pairs_arr.num_pairs[bank2]);
                   // hammer
                    int x =  hammer_thp_prehammer(agg_list, 4, 20);
                   // scan the victims
                   check_victims(bank_pairs_arr.pairs[bank1], bank_pairs_arr.num_pairs[bank1], 0x00);
                   check_victims(bank_pairs_arr.pairs[bank2], bank_pairs_arr.num_pairs[bank2], 0x00);
                  // reset
                  ndx=0;
                  bank1_end = bank1_start+3;
                  bank1_start+=3;
                  bank2_end = bank2_start+3;
                  bank2_start+=3;
               }

           }
       }
   }
}




void main(void){

    


    contig_chunk* chunks = malloc( num_blocks* sizeof(contig_chunk));
    
    for(int i=0; i<num_blocks; i++){
        chunks[i].bank_contig_rows = malloc(16*sizeof(contig_rows));
    }

    

    /// madvise memory///////////////////////////
    int alloc = alloc_buffer(chunks, num_blocks);
  

    for(int i=0; i < num_blocks; i++){
        printf("%d - (%lx, %lx)\n",i,(uint64_t)chunks[i].base, get_physical_addr((uint64_t)chunks[i].base));
        printf("offset %lx\n", chunks[i].virt_offset);
    }

    ///////////////////////////////////
   
    // exit(0);

    ///////////////////////////// HUGE Page 1GB//////////////////////// --- make sure MEM_SIZE is 1gb
    // mem[0] =  (char *) mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_POPULATE| MAP_ANONYMOUS | MAP_HUGETLB| (30 << MAP_HUGE_SHIFT) , -1, 0); //cons 1gb
    // memset(mem[0], 0xFF, MEM_SIZE);
    // if(check_consecutive(mem[0],MEM_SIZE)){
    //     printf("cons 1GB\n");
    // }else{printf("not cons \n");}
    ///////////////////////////////////////////////////


    // For every bank, for every chunk, create a list of contiguous rows 
    
    for(int j=0; j < 16; j++){
        ds_pair* pairs_array = (ds_pair*) malloc(200*sizeof(ds_pair));
        printf("//////////////////////Conflict for Bank %d////////////////////////////\n", j);
        for(int i=0; i < num_blocks; i++){
            printf("-------------Chunk %d------------------------\n", i);
            find_bank_conflict(&chunks[i], j);
        }
        printf("//////////////////////Create Pairs.....................\n");
        int num_pairs = create_hammer_pairs(chunks, j, pairs_array); //create a liist of double sided pairs
        bank_pairs_arr.pairs[j] = pairs_array;
        bank_pairs_arr.num_pairs[j] = num_pairs;
        // int flippy = ten_sided_temp(pairs_array, num_pairs);
    }
    multi_bank();

    

    // char** conflicts = (char**) malloc(sizeof(char*) * 500);
   
    // printf("################################################################################\n");
    // for(int i =0; i< num_blocks; i++){ // this number depends on the memory configuration (how many dimms, ((interleaving)))
    //     printf("-------------------------BASE Page: %d----------------------\n",i);
    //     int num_conf = find_bank_conflict((u_int64_t)chunks[i].base ,MEM_SIZE/PAGE_SIZE,0 ,conflicts, chunks[i].virt_offset);
    //     printf("num conf %d\n", num_conf);
    //     n_sided_temp(conflicts, num_conf);
    //     printf("################################################################################\n");

    // }
}