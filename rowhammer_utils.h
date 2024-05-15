
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <string.h>


#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>



#include <stdio.h>
#include <stdint.h>
#include <sys/statvfs.h>

#include <sys/stat.h>
#include <fcntl.h>


#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <sys/sysinfo.h>


// this struct will hold info on the addresses to hammer
// and their corresponding victim pages
typedef struct {
    u_int64_t agg1;
    u_int64_t agg2;
    u_int32_t num_victims;
    u_int64_t victim_pages[10];
}hammerGroup;

typedef struct {
    u_int64_t BA0;
    u_int64_t BA1;
    u_int64_t BA2;
    u_int64_t BA3;
    u_int64_t channel;
    u_int64_t rank;
    u_int64_t dimm;
    u_int64_t row_num;
}DramAddr;

#define PAGE_SIZE (1<<12)


// change for each system
//Haswell 1 channel 2 dimms
// void dram_address(u_int64_t phys_addr, DramAddr* addr){
//     addr->BA0 = ((phys_addr & 1<<13) >> 13) ^ ((phys_addr & 1<<18) >> 18);
//     addr->BA1 = ((phys_addr & 1<<14) >> 14) ^ ((phys_addr & 1<<19) >> 19);
//     addr->BA2 = ((phys_addr & 1<<17) >> 17) ^ ((phys_addr & 1<<21) >> 21);
//     addr->rank = ((phys_addr & 1<<16) >> 16) ^ ((phys_addr & 1<<20) >> 20);
//     addr->dimm = (phys_addr & 1<<15);
    
//     return;
// }

// stalin addr func from hardware tracker
void dram_address(u_int64_t phys_addr, DramAddr* addr){
    addr->BA0 = ((phys_addr & 1<<6) >> 6) ^ ((phys_addr & 1<<13) >> 13); //done
   
    addr->BA1 = ((phys_addr & 1<<14) >> 14) ^ ((phys_addr & 1<<17) >> 17); //done
    
    addr->BA2 = ((phys_addr & 1<<15) >> 15) ^ ((phys_addr & 1<<18) >> 18); //done
    
    addr->BA3 = ((phys_addr & 1<<16) >> 16) ^ ((phys_addr & 1<<19) >> 19); //done
    
    addr->rank = 0;
    addr->dimm = 0;
    addr->row_num = (phys_addr & 0x3fffe0000) >> 17;
    
    return;
}

inline __attribute__((always_inline)) void
flush_addr(char* addr){
    asm __volatile__(
        "clflush 0(%0)\n\t"
        :
        :"r"(addr)
        :
    );
}

inline __attribute__((always_inline)) uint64_t
rdtscp(void)
{
  uint64_t lo, hi;
  asm volatile("rdtscp\n"
               : "=a"(lo), "=d"(hi)::"%rcx");
  return (hi << 32) | lo;
}


// taken from mojomojo/if_test
// only works when program ran as sudo

u_int64_t get_physical_addr(uintptr_t virtual_addr){
	int fd = open("/proc/self/pagemap", O_RDONLY);
    assert(fd >= 0);

    off_t pos = lseek(fd, (virtual_addr / PAGE_SIZE) * 8, SEEK_SET);
    assert(pos >= 0);
    uint64_t value;
    int got = read(fd, &value, 8);
    assert(got == 8);
    int rc = close(fd);
    assert(rc == 0);

    // Check the "page present" flag.
    assert(value & (1ULL << 63));

    uint64_t frame_num = value & ((1ULL << 54) - 1);
    return (frame_num * PAGE_SIZE) | (virtual_addr & (PAGE_SIZE - 1));
}

void flush_chunck(char* mem, u_int32_t mem_size){
    for(int i =0; i<mem_size; i+=64){
        flush_addr(mem + i);
    }
}



inline __attribute__((always_inline)) uint64_t 
row_conflict_time(u_int64_t a, u_int64_t b){
    u_int64_t time;

    asm volatile(
        "clflush 0(%1)\n\t"
        "clflush 0(%2)\n\t"
        "lfence\n\t"
        "rdtscp\n\t"
        "lfence\n\t"	
        "mov %%rax, %%rdi\n\t"
        "mov (%1), %%rax\n\t"
        "mov (%2), %%rax\n\t"

        "lfence\n\t"
        "rdtscp\n\t"
        "lfence\n\t"
        "sub %%rdi, %%rax\n\t"

        : "=a"(time)
	    :  "b"(a) , "S"(b)
	    : "rdi", "rdx" ,"rcx"  
    );

    return time;

}

inline __attribute__((always_inline)) 
void hammer_asm(char* a, char* b){
    
    asm volatile(
        "clflush 0(%0)\n\t"
        "clflush 0(%1)\n\t"
        "mfence\n\t"
        "mov (%0), %%rax\n\t"
        "mov (%1), %%rax\n\t"

        : 
	    :  "c"(a) , "b"(b)
	    : "rax", "memory"
    );
    return;
}

inline __attribute__((always_inline)) 
void multi_sided(char** a, int num_agg, int togg){

    for (int t=0; t< togg; t++){
        for(int i=0; i< num_agg; i++){
            asm volatile(
                "clflush 0(%0)\n\t"
                "mfence\n\t"
                "mov (%0), %%rax\n\t"

                : 
                :  "c"(a[i]) 
                : "rax", "memory"
            );
        }
    }
    
    
    
    return;
}


inline __attribute__((always_inline)) 
void hammer(char* a, char* b, int toggles){
    for(int i=0; i<toggles; i++){
        hammer_asm(a,b);
        
    }
    return;
}


// from Ingab mjrnr
static inline __attribute__ ((always_inline))
void clflushopt(volatile void *p)
{
	asm volatile ("clflushopt (%0)\n"::"r" (p):"memory");
}



bool check_consecutive(u_int64_t mem, u_int64_t size){
	u_int64_t prev = get_physical_addr(mem);
	u_int64_t phys;
	bool out = true;
	for(u_int64_t i=mem+PAGE_SIZE; i<mem+size; i+= PAGE_SIZE){
		phys = get_physical_addr(i);
        // printf("%lx\n",phys);
		// printf("Vaddr: %lx   Paddr: %lx\n",i, phys);
		if(phys != prev + PAGE_SIZE){
			// printf("NOT consecutive");
			out = false;
		}
		prev  = phys;
	}
	return out;
	
}

static inline __attribute__ ((always_inline))
void mfence()
{
	asm volatile ("mfence":::"memory");
}

int64_t hammer_thp_prehammer(char** v_lst, int skip_iter, int len_aggr)
{
	// printf("\n------Hammer------\n");
	// fprintf(stderr, "prehammer             %d: ", skip_iter);
	// fprintf(out_fd, " prehammer             %d: ", skip_iter);
	// fflush(out_fd);
	int len_dummy = skip_iter;
	// int len_aggr = patt->len;


	// char **v_lst = (char **)malloc(sizeof(char *) * len_aggr);
	// for (size_t i = 0; i < len_aggr; i++)
	// {
	// 	v_lst[i] = thp_dram_2_virt(patt->d_lst[i], patt->v_baselst[i]);
	// }



	// /uint64_t cl0, cl1; // ns
	size_t rounds = 540000;
	// fprintf(stderr, "r: %lu l: %lu \n", patt->rounds, patt->len);

	for (int i = 0; i < 200; i++)
	{
		for (size_t j = 0; j < len_dummy; j += 1)
		{
			*(volatile char *)v_lst[j];
		}
		for (size_t j = 0; j < len_dummy; j += 1)
		{
			clflushopt(v_lst[j]);
		}
		mfence();
	}

	// cl0 = realtime_now();

	for (int i = 0; i < rounds; i++)
	{
		for (size_t j = 0; j < len_aggr; j += 1)
		{
			*(volatile char *)v_lst[j];
		}
		for (size_t j = 0; j < len_aggr; j += 1)
		{
			clflushopt(v_lst[j]);
		}
		mfence();
	}
	// cl1 = realtime_now();

	// free(v_lst);
	// return (cl1-cl0) / 1000000; //ms
	return 0; // ns
}



// __attribute__((always_inline)) __attribute__((optimize("unroll-loops"))) void
// sync_with_ref(int bank_no) { // NK: Detect the refresh in 
// @param
// : bank_no
//   uint64_t start_cycle;
//   uint64_t end_cycle;
//   while (true) {
//     mfence();
//     start_cycle = rdtscp();
//     for (int k = 0; k < no_rows; k++) {
//       *(volatile char *)(addr_list[bank_no][k]);
//     }
//     for (int k = 0; k < no_rows; k++) {
//       asm volatile("clflushopt (%0)"
//                    :
//                    : "r"((volatile char *)(addr_list[bank_no][k]))
//                    : "memory");
//     }
//     end_cycle = rdtscp();
//     if ((end_cycle - start_cycle) > threshold) {
//       // printf("detected ref with: %ld\n",end_cycle-start_cycle);
//       break;
//     }
//   }
// }
// struct histogram_vals {
//   int start_cc;
//   int latency;
// };
// // We hope that most occurring number is the number of rounds we can have
// int findMostOccurring(int *counts, int count_size) {
//   // Normally we would use a hash table but C..
//   int range = 500;      // Safe assumption
//   int frequency[range]; // Array to store the frequency of elements
//   uint64_t sum = 0;
//   for (int i = 0; i < range; i++) {
//     frequency[i] = 0;
//   }
//   for (int i = 0; i < count_size; i++) {
//     if (counts[i] < range) {
//       frequency[counts[i]]++;
//       sum += counts[i];
//     } else
//       printf("something wrong with this data\n");
//   }
//   // Find the most frequent element
//   int max_count = 0;
//   int most_occuring = -1;
//   for (int i = 0; i < range; i++) {
//     if (frequency[i] > max_count) {
//       max_count = frequency[i];
//       most_occuring = i;
//     }
//   }
//   int average = sum / count_size;
//   printf("Average: %d\n", average);
//   return most_occuring;
// }
// int *count_between_outliers(histogram_vals *data, int size, int *count_size,
//                             int no_rows) {
//   int *counts = NULL;
//   uint64_t count = 0;
//   int num_outliers = 0;
//   int between_outliers = 0;
//   int threshold_add = (no_rows < 8) ? no_rows * 5 : no_rows * 10;
//   // threshold = 50 + threshold_add;
//   threshold = 250;
//   int min_val = INT_MAX;
//   for (int i = 0; i < size; i++) {
//     count += data[i].latency;
//     if (data[i].latency < min_val) {
//       min_val = data[i].latency;
//     }
//   }
//   int average = count / (size);
//   count = 0;
//   int outlier_threshold =
//       min_val +
//       threshold; /*average ile mi toplayacagiz yoksa en dusuk degerle mi?*/
//   threshold = outlier_threshold; // globaldeki degeri degistirmek icin.
//   printf("Average: %d -- Min Val: %d -- Outlier threshold to catch REFs: %d\n",
//          average, min_val, outlier_threshold);
//   for (int i = 0; i < size; i++) {
//     if (data[i].latency >= outlier_threshold) {
//       printf("OUTLIER: %d\n", data[i].latency);
//       if (between_outliers) {
//         // If we are already between outliers, we've found the end of an
//         // interval.
//         counts = (int *)realloc(counts, (num_outliers + 1) * sizeof(int));
//         counts[num_outliers++] = count;
//         count = 0;
//       }
//       between_outliers = 1; // Starting to count after this outlier
//     } else if (between_outliers) {
//       count++; // Counting the number of normal latency values
//     }
//   }
//   // If the last value is not an outlier, end the count
//   if (count != 0) {
//     counts = (int *)realloc(counts, (num_outliers + 1) * sizeof(int));
//     counts[num_outliers++] = count;
//   }
//   *count_size = num_outliers; // Set the output parameter to the number of
//                               // intervals counted
//   return counts;              // Return the array of counts
// }
// uint64_t acts_per_ref(HammerSuite *suite) {
//   uint64_t no_of_rounds = 20000;
//   int ref_per_64ms = 0;
//   histogram_vals *histogram =
//       (histogram_vals *)malloc(sizeof(histogram_vals) * no_of_rounds);
//   volatile uint64_t *lat = (uint64_t *)malloc(sizeof(uint64_t) * no_of_rounds);
//   MemoryBuffer *mem = suite->mem;
//   DRAMAddr d_base = suite->d_base;
//   d_base.col = 0;
//   DRAMAddr temp;
//   temp.row = d_base.row + 5;
//   temp.bank = 3;
//   temp.col = 0;
//   addr_list = (char ***)malloc(sizeof(char **) * get_banks_cnt());
//   for (int x = 0; x < get_banks_cnt(); x++) {
//     addr_list[x] = (char **)malloc(sizeof(char *) * no_rows);
//   }
//   for (int x = 0; x < get_banks_cnt(); x++) {
//     temp.bank = x;
//     for (size_t i = 0; i < no_rows; i++) {
//       temp.row = temp.row + 30;
//       addr_list[x][i] = phys_2_virt(dram_2_phys_skx(temp), mem);
//     }
//   }
//   for (uint64_t a = 0; a < no_of_rounds; a++) {
//     lat[a] = 0; // warm up the cache
//   }
//   while (ref_per_64ms < 7000 ||
//          ref_per_64ms > 9800) { // Collect until it is reliable.
//     for (int x = 0; x < 10; x++)
//       sched_yield();
//     // for(int k=0;k<no_rows;k++){
//     //  asm volatile("clflushopt (%0)" : : "r" ((volatile char*)(addr_list[k]))
//     // : "memory");
//     // }
//     uint64_t cl0, cl1;
//     cl0 = realtime_now();
//     uint64_t init_cycles = rdtsc();
//     uint64_t base_to_cut = (init_cycles / 10000000000ULL) * 10000000000ULL;
//     for (int x = 0; x < no_of_rounds; x++) {
//       mfence();
//       for (int k = 0; k < no_rows; k++) {
//         *(volatile char *)(addr_list[3][k]); // 3. banktaki rowlara erisiyoruz
//       }
//       for (int k = 0; k < no_rows; k++) {
//         asm volatile("clflushopt (%0)"
//                      :
//                      : "r"((volatile char *)(addr_list[3][k]))
//                      : "memory");
//       }
//       lat[x] = rdtscp();
//     }
//     cl1 = realtime_now();
//     int time_taken = (cl1 - cl0) / 1000000; // Convert ns to ms
//     // ANALYZE
//     for (int i = 0; i < no_of_rounds; i++) {
//       // printf("i: %ld ----- lat[i]: %ld\n",i, lat[i]);
//       uint64_t res;
//       if (i == 0)
//         res = lat[i] - init_cycles;
//       else
//         res = lat[i] - lat[i - 1];
//       histogram[i].start_cc = lat[i] - base_to_cut;
//       histogram[i].latency = res;
//     }
//     // OUTPUT THE RESULTS
//     FILE *myfile;
//     myfile = fopen("histogram-raw.txt", "w");
//     if (myfile == NULL) {
//       perror("Error opening file");
//       return -1;
//     }
//     for (int i = 0; i < no_of_rounds; i++) {
//       fprintf(myfile, "%d %d\n", histogram[i].start_cc, histogram[i].latency);
//     }
//     fclose(myfile); // Close the file
//     // GET THE NUMBER OF ROUNDS POSSIBLE BETWEEN TWO REFS.
//     int *count_size = (int *)malloc(sizeof(int));
//     int *counts =
//         count_between_outliers(histogram, no_of_rounds, count_size, no_rows);
//     for (int x = 0; x < (*count_size); x++) {
//       printf("count: %d\n", counts[x]);
//     }
//     no_rounds_per_ref = findMostOccurring(counts, *count_size);
//     printf("Most occurring element: %d\n", no_rounds_per_ref);
//     ref_per_64ms = (*count_size * 64) / time_taken;
//     printf("Number of outliers: %d -- Time taken: %d ms -- Ref per 64ms: %d\n",
//            *count_size, time_taken, ref_per_64ms);
//   }
//   return no_rounds_per_ref;
// }

// no_rows = 10;        // no rows to use in ref sync
// acts_per_ref(suite); // Collect statistics about refresh

// sync_with_ref(patt->d_lst[0].bank);
// // TODO: Global
// __attribute__((always_inline))
// __attribute__((optimize("unroll-loops"))) inline void
// sync_with_ref(int bank_no); // NK: Detect the refresh in 
// @param
// : bank_no
// int avg_benchmark = 0;
// int no_rows = 0; // how many rows we accessed while calculating the number of
//                  // rounds possible each ref
// int no_rounds_per_ref = 0; // how many rounds we can hammer with no_rows.** e.g.
//                            // 12 rounds with 10 rows **
// int threshold = 0;         // threshold to detect refreshes with no_rows.
// char ***addr_list = NULL; // dummy addresses used in detecting refreshes. size: no_rows

