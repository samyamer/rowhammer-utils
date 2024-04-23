#include "rowhammer_utils.h"
#define ROW_CONFLICT_TH 470


// finds all pages that row conflict with the base_page
int find_bank_conflict(u_int64_t mem, int num_pages, u_int64_t base_page, char** conflict_arr){
    
    u_int64_t base = mem + (base_page*PAGE_SIZE);
    u_int64_t curr;
    uint64_t phys;
    int ndx=0;

    DramAddr* dram = (DramAddr*) malloc(sizeof(DramAddr));
    phys = get_physical_addr(base);
    dram_address(phys, dram);

    printf("Base page memory\n");
   
    printf("BA0:%lx BA1:%lx BA2:%lx BA3:%lx row:%lx\n", dram->BA0,dram->BA1, dram->BA2, dram->BA3, dram->row_num);
    printf("%lx\n",phys);
    printf("----------------------------\n");
    printf("starting row conflict\n");
    for(int i=0; i<num_pages; i++){
        for (int v=0; v< 2; v++){
            curr = mem + (i*PAGE_SIZE) + 64*v;
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
                printf("BA0:%lx BA1:%lx BA2:%lx BA3:%lx row:%lx\n", dram->BA0,dram->BA1, dram->BA2, dram->BA3, dram->row_num);
                printf("%lx\n",phys);
                printf("%lu\n",acc_time);
                printf("----------------------------\n");
                // add to array
                conflict_arr[ndx] = (char*)curr;
                ndx++;

            }
        }
        

    }
    free(dram);
    return ndx;
}


// on stalin (ddr4) each row has data from 4 pages/
// so, in conflict array, every 4 entries correspond to same row
// In the conflict array, the first two entries are pages with data in that row from  odd blocks (1,3,5...)
// the second two entries are pages with data in even blocks (0,2,4,...)
int memory_template(char** conflicts, int num_conf){
    // victim index will step by 2 from 2 to n-4
    int victim_ndx =4;

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

      
        // agg_list[0] = conflicts[victim_ndx -4];
        // agg_list[1] = conflicts[victim_ndx +4];
        // agg_list[2] = conflicts[victim_ndx +8];
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





void main(void){

    int num_pages=(1<<9);
    char* mem;
    do{
            mem =  (char *) mmap(NULL, num_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE| MAP_POPULATE, -1, 0);
    }while(!check_consecutive(mem,num_pages*PAGE_SIZE));


    char** conflicts = (char**) malloc(sizeof(char*) * 100);
   
    printf("################################################################################\n");
    for(int i =0; i< 64; i+=2){ // this number depends on the memory configuration (how many dimms, ((interleaving)))
        printf("-------------------------BASE Page: %d----------------------\n",i);
        int num_conf = find_bank_conflict((u_int64_t)mem,num_pages,i ,conflicts);
        int tgt = memory_template(conflicts, num_conf);
        printf("################################################################################\n");

    }
}