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
#include "sparse_formats.c"

//#define USEGPU 1
static struct option long_options[] = {
      /* name, has_arg, flag, val */
      {"cpu", 0, NULL, 'c'},
      {"device", 1, NULL, 'd'},
      {"verbose", 0, NULL, 'v'},
      {"csr_file",1,NULL,'f'},
      {"print",0,NULL,'p'},
      {"affirm",0,NULL,'a'},
      {0,0,0,0}
};

int platform_id=PLATFORM_ID, n_device=DEVICE_ID;

int main(int argc, char** argv)
{
	cl_int err;
	int usegpu = USEGPU;
    int be_verbose = 0,do_print=0,do_affirm=0;
    unsigned long density_ppm = 500000;
    unsigned int N = 512;
    int opt, option_index=0,i;
    char* file_path = NULL;

    unsigned int correct;

    const char* usage = "Usage: %s -f <file_path> [-v] [-c] [-p] [-a]\n\n \
    		-f: Read CSR Matrix from file <file_path>\n \
    		-v: Be Verbose \n \
    		-c: use CPU\n \
    		-p: Print matrices to stdout in standard (2-D Array) format - Warning: lots of output\n \
    		-a: Affirm results with serial C code on CPU\n\n";

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

    while ((opt = getopt_long(argc, argv, "::vcf:pa::", long_options, &option_index)) != -1 )
    {
    	switch(opt)
		{
    		case 'v':
    			printf("verify\n");
    			be_verbose = 1;
    			break;
			case 'c':
				printf("using cpu\n");
				usegpu = 0;
				break;
			case 'f':
				if(optarg != NULL)
					file_path = optarg;
				else
					file_path = argv[optind];
				printf("Reading Input from '%s'\n",file_path);
				break;
			case 'p':
				do_print = 1;
				break;
			case 'a':
				do_affirm = 1;
				break;
			default:
				fprintf(stderr,"Illegal Argument: '%c'\n\n",opt);
				fprintf(stderr, usage,argv[0]);
				exit(EXIT_FAILURE);
		  }
    }

    if(!file_path)
	{
    	fprintf(stderr,"-f Option must be supplied\n\n",opt);
		fprintf(stderr, usage,argv[0]);
		exit(EXIT_FAILURE);
	}
    csr_matrix csr;
    read_csr(&csr,file_path);

    if(do_print) print_csr_std(&csr,stdout);

    //The other arrays
    float * x_host = float_new_array(csr.num_cols);
    float * y_host = float_new_array(csr.num_rows);

    unsigned int ii;
    for(ii = 0; ii < csr.num_cols; ii++)
    {
        x_host[ii] = rand() / (RAND_MAX + 1.0);
        if(do_print) printf("x[%d] = %6.2f\n",ii,x_host[ii]);
    }
    for(ii = 0; ii < csr.num_rows; ii++)
    {
        y_host[ii] = rand() / (RAND_MAX + 2.0);
        if(do_print) printf("y[%d] = %6.2f\n",ii,y_host[ii]);
    }

    if(be_verbose) printf("input generated.\n");

    /* Retrieve an OpenCL platform */
    device_id = GetDevice(platform_id, n_device,usegpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_CPU);

    if(be_verbose) ocd_print_device_info(device_id);

    /* Create a compute context */
    context = clCreateContext(0, 1, &device_id, NULL, NULL, &err);
    CHKERR(err, "Failed to create a compute context!");

    /* Create a command queue */
    commands = clCreateCommandQueue(context, device_id, CL_QUEUE_PROFILING_ENABLE, &err);
    CHKERR(err, "Failed to create a command queue!");

    /* Load kernel source */
    kernelFile = fopen("spmv_csr_kernel.cl", "r");
    if(kernelFile == NULL)
    	fprintf(stderr,"Cannot Open Kernel.\n");
    else
    	if(be_verbose) printf("Kernel Opened.\n");
    fseek(kernelFile, 0, SEEK_END);
    if(be_verbose) printf("Seeked to kernel end.\n");
    kernelLength = (size_t) ftell(kernelFile);
    if(be_verbose) printf("Kernel Source Read.\n");
    kernelSource = (char *) malloc(sizeof(char)*kernelLength);
    if(kernelSource == NULL)
    	fprintf(stderr,"Heap Overflow. Cannot Load Kernel Source.\n");
    else
    	if(be_verbose) printf("Memory Allocated.\n");
    rewind(kernelFile);
    lengthRead = fread((void *) kernelSource, kernelLength, 1, kernelFile);
    fclose(kernelFile);

    if(be_verbose) printf("kernel source loaded.\n");

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

    if(be_verbose) printf("kernel created\n");

    /* Create the input and output arrays in device memory for our calculation */
    size_t csr_ap_bytes,csr_aj_bytes,csr_ax_bytes,x_loc_bytes,y_loc_bytes;
    csr_ap_bytes = sizeof(unsigned int)*csr.num_rows+4;
    if(be_verbose) printf("Allocating %zu bytes for csr_ap...\n",csr_ap_bytes);
    csr_ap = clCreateBuffer(context, CL_MEM_READ_ONLY,csr_ap_bytes, NULL, &err);
    CHKERR(err, "Failed to allocate device memory for csr_ap!");

    csr_aj_bytes = sizeof(unsigned int)*csr.num_nonzeros;
    if(be_verbose) printf("Allocating %zu bytes for csr_aj...\n",csr_aj_bytes);
    csr_aj = clCreateBuffer(context, CL_MEM_READ_ONLY, csr_aj_bytes, NULL, &err);
    CHKERR(err, "Failed to allocate device memory for csr_aj!");

    csr_ax_bytes = sizeof(float)*csr.num_nonzeros;
    if(be_verbose) printf("Allocating %zu bytes for csr_ax...\n",csr_ax_bytes);
    csr_ax = clCreateBuffer(context, CL_MEM_READ_ONLY, csr_ax_bytes, NULL, &err);
    CHKERR(err, "Failed to allocate device memory for csr_ax!");

    x_loc_bytes = sizeof(float)*csr.num_cols;
    if(be_verbose) printf("Allocating %zu bytes for x_loc...\n",x_loc_bytes);
    x_loc = clCreateBuffer(context, CL_MEM_READ_ONLY, x_loc_bytes, NULL, &err);
    CHKERR(err, "Failed to allocate device memory for x_loc!");

    y_loc_bytes = sizeof(float)*csr.num_rows;
    if(be_verbose) printf("Allocating %zu bytes for y_loc...\n",y_loc_bytes);
    y_loc = clCreateBuffer(context, CL_MEM_READ_ONLY, y_loc_bytes, NULL, &err);
    CHKERR(err, "Failed to allocate device memory for x_loc!");

    if(be_verbose) printf("buffers created\n");

    /* beginning of timing point */
    stopwatch_start(&sw); 

    if(be_verbose) printf("stopwatch started\n");
   
    /* Write our data set into the input array in device memory */
	err = clEnqueueWriteBuffer(commands, csr_ap, CL_TRUE, 0, sizeof(unsigned int)*csr.num_rows+4, csr.Ap, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	if(be_verbose) printf("Ap Buffer Enqueued\n");
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, csr_aj, CL_TRUE, 0, sizeof(unsigned int)*csr.num_nonzeros, csr.Aj, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	if(be_verbose) printf("Aj Buffer Enqueued\n");
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, csr_ax, CL_TRUE, 0, sizeof(float)*csr.num_nonzeros, csr.Ax, 0, NULL, &ocdTempEvent);  //OOM kill here
	clFinish(commands);
	if(be_verbose) printf("Ax Buffer Enqueued\n");
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, x_loc, CL_TRUE, 0, sizeof(float)*csr.num_cols, x_host, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	if(be_verbose) printf("X_host Buffer Enqueued\n");
	START_TIMER(ocdTempEvent, OCD_TIMER_H2D, "CSR Data Copy", ocdTempTimer)
	END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to write to source array!");
    err = clEnqueueWriteBuffer(commands, y_loc, CL_TRUE, 0, sizeof(float)*csr.num_rows, y_host, 0, NULL, &ocdTempEvent);
	clFinish(commands);
	if(be_verbose) printf("Y_host Buffer Enqueued\n");
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

    if(be_verbose) printf("set kernel arguments\n");

    /* Get the maximum work group size for executing the kernel on the device */
    err = clGetKernelWorkGroupInfo(kernel, device_id, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), (void *) &local_size, NULL);
    if(be_verbose) printf("Kernel Max Work Group Size: %d\n",local_size);
    CHKERR(err, "Failed to retrieve kernel work group info!");

    /* Execute the kernel over the entire range of our 1d input data set */
    /* using the maximum number of work group items for this device */
    global_size = csr.num_rows;
    int num_wg;
    if(global_size % local_size != 0)
    {
		num_wg = global_size / local_size + 1;
		local_size = global_size / (num_wg);
    }
    if(be_verbose) printf("globalsize: %d - num_wg: %d - local_size: %d\n",global_size,num_wg,local_size);
    err = clEnqueueNDRangeKernel(commands, kernel, 1, NULL, &global_size, &local_size, 0, NULL, &ocdTempEvent);
    clFinish(commands);
	START_TIMER(ocdTempEvent, OCD_TIMER_KERNEL, "CSR Kernel", ocdTempTimer)
    END_TIMER(ocdTempTimer)
    CHKERR(err, "Failed to execute kernel!");


    if(be_verbose) printf("kernel executed\n");

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

   if(do_print)
   {
       for (i = 0; i < csr.num_rows; i++)
           printf("row: %d	output: %6.2f \n", i, output[i]);
   }

   if(do_affirm)
   {
	   if(be_verbose) printf("Validating results with serial C code on CPU...\n");
	   int row,next_nz_row;
	   float sum = 0;
	   int row_start = 0;
	   int row_end = 0;
	   for(row=0; row < csr.num_rows; row++)
	   {
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
   }

    /* Print a brief summary detailing the results */
    ocd_finalize();

    if(be_verbose) printf("ocd finalized\n");

    /* Shutdown and cleanup */
    free_csr(&csr);
    clReleaseMemObject(csr_ap);
    if(be_verbose) printf("Released csr_ap\n");
    clReleaseMemObject(csr_aj);
    if(be_verbose) printf("Released csr_aj\n");
    clReleaseMemObject(csr_ax);
    if(be_verbose) printf("released csr_az\n");
    clReleaseMemObject(x_loc);
    if(be_verbose) printf("released x_loc\n");
    clReleaseMemObject(y_loc);
    if(be_verbose) printf("released y_loc\n");
    clReleaseProgram(program);
    if(be_verbose) printf("released program\n");
    cl_int kernel_release_err = clReleaseKernel(kernel);
    if(kernel_release_err != CL_SUCCESS) printf("Error: %d",kernel_release_err);
    if(be_verbose) printf("released kernel\n");
    clReleaseCommandQueue(commands);
    if(be_verbose) printf("released commands\n");
    clReleaseContext(context);
    if(be_verbose) printf("released context\n");
    return 0;
}

