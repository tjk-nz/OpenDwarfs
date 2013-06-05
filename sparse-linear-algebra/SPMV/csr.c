#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include "../../include/rdtsc.h"
#include "../../include/common_args.h"


#define CHKERR(err, str) \
    if (err != CL_SUCCESS) \
    { \
        fprintf(stderr, "CL Error %d: %s\n", err, str); \
        exit(1); \
    }

#include "common.h"
#include "common.c"
#include "sparse_formats.h"
//#define USEGPU 1
static struct option long_options[] = {
      /* name, has_arg, flag, val */
      {"cpu", 0, NULL, 'c'},
      {"device", 1, NULL, 'd'},
      {"platform", 1, NULL, 'p'},
      {"verify", 0, NULL, 'v'},
      {"density",1,NULL, 'D'},
      {"size",1,NULL, 'N'},
      {0,0,0,0}
};

int platform_id=PLATFORM_ID, n_device=DEVICE_ID;

int main(int argc, char** argv)
{
	cl_int err;
	int usegpu = USEGPU;
    int do_verify = 0;
    unsigned long density_ppm = 19;
    unsigned int N = 512*512;
    int opt, option_index=0;

    unsigned int correct;

    const char* usage = "Usage: %s [-v] [-c] [-D <xx>] [-N <xx>]\n\n \
    		-v: Warning: lots of output\n \
    		-c: use CPU\n \
    		-D: Density (fraction of Non-Zero Elements) of test matrix expressed in parts per million\n \
    		-N: Size (Length and Width) of square test matrix\n\n";

    size_t global_size;
    size_t local_size;

    cl_device_id device_id;
    cl_context context;
    cl_command_queue commands;
    cl_program program;
    cl_kernel kernel;

    stopwatch sw;

    cl_mem csr_ap;
    cl_mem csr_aj;
    cl_mem csr_ax;
    cl_mem x_loc;
    cl_mem y_loc;

    FILE *kernelFile;
    char *kernelSource;
    size_t kernelLength;
    size_t lengthRead;

    ocd_init(&argc, &argv, NULL);
    ocd_options opts = ocd_get_options();
    platform_id = opts.platform_id;
    n_device = opts.device_id;

    while ((opt = getopt_long(argc, argv, "::vcD:N:::", long_options, &option_index)) != -1 )
    {
    	switch(opt)
		{
    		case 'v':
    			fprintf(stderr, "verify\n");
    			do_verify = 1;
    			break;
			case 'c':
				fprintf(stderr, "using cpu\n");
				usegpu = 0;
				break;
			case 'D':
				if(optarg != NULL)
					density_ppm = atol(optarg);
				else
					density_ppm = atol(argv[optind]);
				printf("Density = %u / 1000000\n",density_ppm);
				break;
			case 'N':
				if(optarg != NULL)
					N = atoi(optarg);
				else
					N = atoi(argv[optind]);
				printf("N = %d\n",N);
				break;
			default:
			  fprintf(stderr, usage,argv[0]);
			  exit(EXIT_FAILURE);
		  }
    }

    /* Fill input set with random float values */
    int i;

    coo_matrix coo;
    coo = rand_square_coo(N,density_ppm);
    csr_matrix csr;
    csr = coo_to_csr(&coo);

    //The other arrays
    float * x_host = float_new_array(csr.num_cols);
    float * y_host = float_new_array(csr.num_rows);

    unsigned int ii;
    for(ii = 0; ii < csr.num_cols; ii++){
        x_host[ii] = rand() / (RAND_MAX + 1.0);
    }
    for(ii = 0; ii < csr.num_rows; ii++){
        y_host[ii] = rand() / (RAND_MAX + 2.0);
    }

//    if(do_verify)
    	printf("input generated.\n");

    /* Retrieve an OpenCL platform */
    device_id = GetDevice(platform_id, n_device,usegpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_CPU);

    /* Create a compute context */
    context = clCreateContext(0, 1, &device_id, NULL, NULL, &err);
    CHKERR(err, "Failed to create a compute context!");

    /* Create a command queue */
    commands = clCreateCommandQueue(context, device_id, CL_QUEUE_PROFILING_ENABLE, &err);
    CHKERR(err, "Failed to create a command queue!");

    if(do_verify) printf("changes made.\n");

    /* Load kernel source */
    kernelFile = fopen("spmv_csr_kernel.cl", "r");
    if(kernelFile == NULL)
    	fprintf(stderr,"Cannot Open Kernel.\n");
    else
    	if(do_verify) printf("Kernel Opened.\n");
    fseek(kernelFile, 0, SEEK_END);
    if(do_verify) printf("Seeked to kernel end.\n");
    kernelLength = (size_t) ftell(kernelFile);
    if(do_verify) printf("Kernel Source Read.\n");
    kernelSource = (char *) malloc(sizeof(char)*kernelLength);
    if(kernelSource == NULL)
    	fprintf(stderr,"Heap Overflow. Cannot Load Kernel Source.\n");
    else
    	if(do_verify)
    		printf("Memory Allocated.\n");
    rewind(kernelFile);
    lengthRead = fread((void *) kernelSource, kernelLength, 1, kernelFile);
    fclose(kernelFile);

    if(do_verify)
      printf("kernel source loaded.\n");

    /* Create the compute program from the source buffer */
    program = clCreateProgramWithSource(context, 1, (const char **) &kernelSource, &kernelLength, &err);
    CHKERR(err, "Failed to create a compute program!");

    /* Free kernel source */
    free(kernelSource);

    /* Build the program executable */
    err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (err == CL_BUILD_PROGRAM_FAILURE)                                                                                                                                       
    {
        char *buildLog;
        size_t logLen;
        err = clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &logLen);
        buildLog = (char *) malloc(sizeof(char)*logLen);
        err = clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, logLen, (void *) buildLog, NULL);
        fprintf(stderr, "CL Error %d: Failed to build program! Log:\n%s", err, buildLog);
        free(buildLog);
        exit(1);
    }
    CHKERR(err, "Failed to build program!");

    /* Create the compute kernel in the program we wish to run */
    kernel = clCreateKernel(program, "csr", &err);
    CHKERR(err, "Failed to create a compute kernel!");


    if(do_verify)
      printf("kernel created\n");

    /* Create the input and output arrays in device memory for our calculation */
    csr_ap = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(unsigned int)*csr.num_rows+4, NULL, &err);
    CHKERR(err, "Failed to allocate device memory!");
    csr_aj = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(unsigned int)*csr.num_nonzeros, NULL, &err);
    CHKERR(err, "Failed to allocate device memory!");
    csr_ax = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(float)*csr.num_nonzeros, NULL, &err);
    CHKERR(err, "Failed to allocate device memory!");
    x_loc = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(float)*csr.num_cols, NULL, &err);
    CHKERR(err, "Failed to allocate device memory!");
    y_loc = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(float)*csr.num_rows, NULL, &err);
    CHKERR(err, "Failed to allocate device memory!");


    if(do_verify)
      printf("buffers created\n");

    /* beginning of timing point */
    stopwatch_start(&sw); 


    if(do_verify)
      printf("stopwatch started\n");
   
    /* Write our data set into the input array in device memory */
	err = clEnqueueWriteBuffer(commands, csr_ap, CL_TRUE, 0, sizeof(unsigned int)*csr.num_rows+4, csr.Ap, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, csr_aj, CL_TRUE, 0, sizeof(unsigned int)*csr.num_nonzeros, csr.Aj, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, csr_ax, CL_TRUE, 0, sizeof(float)*csr.num_nonzeros, csr.Ax, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, x_loc, CL_TRUE, 0, sizeof(float)*csr.num_cols, x_host, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, y_loc, CL_TRUE, 0, sizeof(float)*csr.num_rows, y_host, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
	END_TIMER(ocdTempTimer)
    /* Set the arguments to our compute kernel */
    err = 0;
    err = clSetKernelArg(kernel, 0, sizeof(unsigned int), &csr.num_rows);
    err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &csr_ap);
    err |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &csr_aj);
    err |= clSetKernelArg(kernel, 3, sizeof(cl_mem), &csr_ax);
    err |= clSetKernelArg(kernel, 4, sizeof(cl_mem), &x_loc);
    err |= clSetKernelArg(kernel, 5, sizeof(cl_mem), &y_loc);
    CHKERR(err, "Failed to set kernel arguments!");


    if(do_verify)
      printf("set kernel arguments\n");

    /* Get the maximum work group size for executing the kernel on the device */
    err = clGetKernelWorkGroupInfo(kernel, device_id, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), (void *) &local_size, NULL);
    if(do_verify)
    	printf("Device Max Local Size: %d\n",local_size);
    CHKERR(err, "Failed to retrieve kernel work group info!");

    /* Execute the kernel over the entire range of our 1d input data set */
    /* using the maximum number of work group items for this device */
    global_size = csr.num_rows;
    err = clEnqueueNDRangeKernel(commands, kernel, 1, NULL, &global_size, &local_size, 0, NULL, &ocdTempEvent);
    clFinish(commands);
    CHKERR(err, "Failed to execute kernel!");

    if(do_verify)
      printf("NDRange enqueued\n");

	START_TIMER(ocdTempEvent, OCD_TIMER_KERNEL, "CSR Kernel", ocdTempTimer)


    if(do_verify)
      printf("timer started\n");

    END_TIMER(ocdTempTimer)
//    CHKERR(err, "Failed to execute kernel!");


    if(do_verify)
      printf("kernel executed\n");

    /* Wait for the command commands to get serviced before reading back results */
    float output[csr.num_rows];
    
    /* Read back the results from the device to verify the output */
	err = clEnqueueReadBuffer(commands, y_loc, CL_TRUE, 0, sizeof(float)*csr.num_rows, output, 0, NULL, &ocdTempEvent);
    clFinish(commands);
    START_TIMER(ocdTempEvent, OCD_TIMER_D2H, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
	CHKERR(err, "Failed to read output array!");

    /* end of timing point */
    stopwatch_stop(&sw);
    printf("Time consumed(ms): %lf Gflops: %f \n", 1000*get_interval_by_sec(&sw), (2.0 * (double) csr.num_nonzeros / get_interval_by_sec(&sw)) / 1e9);


   /* Validate our results */
   if(do_verify){
       for (i = 0; i < csr.num_rows; i++){
           printf("row: %d	output: %f \n", i, output[i]);  
       }
   }

   int row = 0;
   float sum = 0;
   int row_start = 0;
   int row_end = 0;
   for(row =0; row < csr.num_rows; row++){     
        sum = y_host[row];
        
        row_start = csr.Ap[row];
        row_end   = csr.Ap[row+1];
        
        unsigned int jj = 0;
        for (jj = row_start; jj < row_end; jj++){             
            sum += csr.Ax[jj] * x_host[csr.Aj[jj]];      
        }
        y_host[row] = sum;
    }
    for (i = 0; i < csr.num_rows; i++){
        if((fabsf(y_host[i]) - fabsf(output[i])) > .001)
             printf("Possible error, difference greater then .001 at row %d \n", i);
    }

    /* Print a brief summary detailing the results */
    ocd_finalize();

    /* Shutdown and cleanup */
    clReleaseMemObject(csr_ap);
    clReleaseMemObject(csr_aj);
    clReleaseMemObject(csr_ax);
    clReleaseMemObject(x_loc);
    clReleaseMemObject(y_loc);
    clReleaseProgram(program);
    clReleaseKernel(kernel);
    clReleaseCommandQueue(commands);
    clReleaseContext(context);
    return 0;
}
