/*
    Header to talk to GPU
 */

#ifndef _GPU_MEM_UTIL_H_
#define _GPU_MEM_UTIL_H_
/*
#ifdef __cplusplus
extern "C" {
#endif
*/
void print_gpu_devices_info(void);  
/*
 * Memory allocation on CPU or GPU according to HAVE_CUDA pre-compile option and use_cuda flag
 *
 * returns: a pointer to the allocated buffer or NULL on error
 */
void *work_buffer_alloc(size_t length, int use_cuda, const char *bdf);

/*
 * CPU or GPU memory free, according to HAVE_CUDA pre-compile option and use_cuda flag
 */
void work_buffer_free(void *buff, int use_cuda);

/*
#ifdef __cplusplus
}
#endif
*/
#endif /* _GPU_MEM_UTIL_H_ */
