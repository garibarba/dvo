/**
 * \file
 * \brief   Class implementing the paper.
 *
 * \author  Oskar Carlbaum, Guillermo Gonzalez de Garibay, Georg Kuschk
 * \date    April 2016
 *
 * A class instance is created passing the first frame. Subsequent calls to the
 * align function, providing each time a new frame, return the transformation from
 * the last frame to the starting frame.
 */

#include <Eigen/Dense>
#include "preprocessing.cuh"
#include "lieAlgebra.hpp"
#include "alignment.cuh"
#include "common.h"
// cuBLAS
#define CUDA_API_PER_THREAD_DEFAULT_STREAM
#include <cuda_runtime.h>

#ifdef ENABLE_CUBLAS
        #include "cublas_v2.h"
#endif
// #include <string> // for 'debugging'

enum SolvingMethod { GAUSS_NEWTON, LEVENBERG_MARQUARDT, GRADIENT_DESCENT };
// enum ResidualWeight { NONE, HUBER, TDIST };

class Tracker {

public:

/**
 * Tracker constructor
 * @param grayFirstFrame        gray image array of floats (not uchar)
 * @param depthFirstFrame       depth image array of floats
 * @param K                     Eigen 3x3 matrix with camera projection parameters
 * @param solvingMethod         Method for solving the linear system for delta ksi
 * @param weightType            Weight shape for the residuals // TODO? guillermo: not sure how far this is implemented
 * @param minLevel              Lowest level index in the pyramid to be used for alignment. Default is 0 (full resolution).
 * @param maxLevel              Highest level index of the pyramid above the full resolution image
 * @param maxIterationsPerLevel Maximum number of iterations per pyramid level
 */
Tracker(
        float* grayFirstFrame,
        float* depthFirstFrame,
        int width,
        int height,
        Eigen::Matrix3f K,
        int minLevel = 0,
        int maxLevel = 4,
        bool useTDistWeights = true,
        int maxIterationsPerLevel = 20,
        SolvingMethod solvingMethod = GAUSS_NEWTON
        // ResidualWeight weightType = NONE,
        ) :
        width(width),
        height(height),
        solvingMethod(solvingMethod),
        minLevel(minLevel),
        maxLevel(maxLevel),
        maxIterationsPerLevel(maxIterationsPerLevel),
        xi(Vector6f::Zero()),
        xi_total(Vector6f::Zero()),
        A(Matrix6f::Zero()),
        b(Vector6f::Zero()),
        useTDistWeights(useTDistWeights)
        // weightType(weightType)
{
        cudaDeviceSynchronize();  CUDA_CHECK;

        // Get information about the GPU
        cudaGetDevice(&devID); CUDA_CHECK;
        cudaGetDeviceProperties(&props, devID); CUDA_CHECK;
        g_CUDA_maxSharedMemSize = props.sharedMemPerBlock;

        // Create additional streams
        for(int i = 0; i < NUM_STREAMS ; i++) {
            cudaStreamCreate ( &streams[i] );
        }


#ifdef ENABLE_CUBLAS
        stat = cublasCreate(&handle);
        if (stat != CUBLAS_STATUS_SUCCESS) {
                printf ("\n\n!---------------cuBLAS initialization failed---------------!\n\n");
                //return EXIT_FAILURE;
        }
        else printf ("\n\n!--------------cuBLAS initialization succesful--------------!\n\n");
#endif

        // make pyramid vector large enough to hold all levels
        d_cur.resize(maxLevel+1);
        d_prev.resize(maxLevel+1);

        // Create Buffers
        allocateGPUMemory();

        // allocate intrinisc matrix (camera projection) for all pyramid levels
        K_pyr.resize(maxLevel+1);
        K_inv_pyr.resize(maxLevel+1);

        fill_K_levels(K);

        load_K_to_device();

        // Fill image pyramid of the first frame. Already as previous frame since align fills the current frame d_cur and only swaps at the end.
        fill_pyramid(d_prev, grayFirstFrame, depthFirstFrame);

        define_texture_parameters();
}

/**
 * destructor
 */
~Tracker() {
#ifdef ENABLE_CUBLAS
        cublasDestroy(handle);
#endif
        deallocateGPUMemory();
}

/**
 * Provide a new frame to the tracker and calculate the transition from the previous
 * @param  grayCur  New (current) gray image of floats. Used as second frame.
 * @param  depthCur New (current) depth image of floats. It gets processed into a pyramid but its values are not actually used until the next call to align.
 * @return          Minimal transformation representation in twist coordinates. Optimal warp of the previous gray and depth onto the new (current) image.
 */
Vector6f align(float *grayCur, float *depthCur) {
        fill_pyramid(d_cur, grayCur, depthCur);


        // Use the previous xi as initial guess. It is stored in a private variable
        // other option, initialize as 0:
        // xi = Vector6f::Zero();

        // from the highest level to the minimum level set
        for (int level = maxLevel; level >= minLevel; level--) {
                // std::cout << "Level: " << level << std::endl;

                // calculate size of image in current level
                int level_width = width / (1 << level); // calculating bitwise the succesive powers of 2
                int level_height = height / (1 << level);

                // set d_prev_err to big float number
                float error_prev = BIG_FLOAT; // initialize as a great value to make sure loop continues for at least one iteration below

                bind_textures(level, level_width, level_height); // used for interpolation in the current image

                // T distribution variance, (initial)
                // declared even if not needed (no weights)
                float variance = VARIANCE_INITIAL;

                //cudaMemcpy(d_sigma, &SIGMA_INITIAL, sizeof(float), cudaMemcpyHostToDevice ); CUDA_CHECK;

                // for a maximum number of iterations per level
                for (int i = 0; i < maxIterationsPerLevel; i++) {
                        // std::cout << "Iteration #" << i ;

                        // Calculate Rotation matrix and translation vector: CPU operation
                        convertSE3ToT(xi, R, t);

                        // calculate RK_inv = R*K_inv in CPU and copy to constant memory (both rotation and translation)
                        RK_inv = R * K_inv_pyr[level];
                        cudaMemcpyToSymbol (const_RK_inv, RK_inv.data(), 9*sizeof(float)); CUDA_CHECK;
                        cudaMemcpyToSymbol (const_translation, t.data(), 3*sizeof(float)); CUDA_CHECK;

                        // transform_points: CUDA operation
                        transform_points(level, level_width, level_height);

                        // parallel CUDA kernels: two streams
                                // calculate_jacobian J(n,6)  // calculate_residuals r_xi(n,1) and error (mean squares of r_xi)
                                                              // calculate_weights W(n,1)
                        calculate_residuals(level, level_width, level_height); //, stream2); // +3ms
                        calculate_jacobian(level, level_width, level_height); //, stream1);  // +7ms
                        calculate_error(level, level_width, level_height); //, stream2); // +6m      // cudamalloc is synchronous, so this does not work fine with parallel?

                        // variance is actually not used, but has to be passed by reference to keep
                        // its value for next iteration, and referenced variables cannot get set to a default valu
                        calculate_weights(level, level_width, level_height, variance, useTDistWeights); //, stream2);   // +1ms

                        // parallel CUDA kernels: two streams
                                // calculate A(6,6) = J.T * W * J   // calculate B(6,1) = -J.T * W * r
                                // TODO: best order to calculate the previous multiplications. J.T * W first? (used by both) or all together avoiding reads?
                                // probably A and be separated are better. Less R&W. Less code.
                        calculate_A ( level, level_width, level_height ); //, stream1 );
                        calculate_b ( level, level_width, level_height ); //, stream1 );

                        cudaDeviceSynchronize();
                        // solve linear system: A * delta_xi = b; with solver of Eigen library: CPU operation.      TODO: Faster to solve directly in GPU?
                        xi_delta = -(A.ldlt().solve(b)); // Solve using Cholesky LDLT decomposition

                        // Convert the twist coordinates in xi & xi_delta to transformation matrices using Lie algebra
                        // Convert the combined transformation matrix back to twist coordinates
                        xi = lieLog(lieExp(xi_delta) * lieExp(xi));

                        // error is the sum of the squared residuals
                        cudaMemcpy(&error, d_error, sizeof(float), cudaMemcpyDeviceToHost); CUDA_CHECK;
                        // error /= n; // not needed because n is always the same

                        // if the change in error is very small, break iterations loop and go to higher resolution in pyramid
                        if (error / error_prev > 0.995 || error == 0) break;

                        error_prev = error;

                        // DEBUG BLOCK
                        {
                        // DEBUG --- PLOT RESIDUAL IMAGES
                        // if(level < 2){
                        //     cv::Mat mTest(level_height, level_width, CV_32FC1);
                        //     cv::Mat mTest2(level_height, level_width, CV_32FC1);
                        //     float *prev = new float[level_width*level_height];
                        //     cudaMemcpy(prev, d_r, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
                        //     convert_layered_to_mat(mTest, prev);
                        //     mTest2 = cv::abs(mTest);
                        //     //cv::convertScaleAbs(mTest, mTest);
                        //     showImage( "Residual at level: " + std::to_string(level) + ", iteration: " + std::to_string(i) , mTest2, 300, 100); cv::waitKey(0);
                        // /*    cudaMemcpy(prev, d_W, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
                        //     convert_layered_to_mat(mTest, prev);
                        //     double min, max;
                        //     cv::minMaxLoc(mTest, &min, &max);
                        //     showImage( "Weights at level: " + std::to_string(level) + ", iteration: " + std::to_string(i) , mTest/max, 300, 100); cv::waitKey(0);
                        // */
                        // }

                        // // DEBUG display image
                        // cv::Mat mTest(level_height, level_width, CV_32FC1);
                        // float *res = new float[level_width*level_height];
                        // cudaMemcpy(res, d_r, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
                        // convert_layered_to_mat(mTest, res);
                        // mTest = cv::abs(mTest);
                        // double min, max;
                        // cv::minMaxLoc(mTest, &min, &max);
                        // showImage( "Depth: " + std::to_string(level), mTest, 100, 100); cv::waitKey(0);
                        // // DEBUG print out values
                        // if (i==0) {
                        //     // dim3  dimBlock( g_CUDA_blockSize2DX, g_CUDA_blockSize2DY, 1 );
                        //     //
                        //     // // Grid = 2D array of blocks
                        //     // // gridSizeX = ceil( width / nBlocksX )
                        //     // // gridSizeY = ceil( height / nBlocksX )
                        //     // int   gridSizeX = (level_width  + dimBlock.x-1) / dimBlock.x;
                        //     // int   gridSizeY = (level_height + dimBlock.y-1) / dimBlock.y;
                        //     // dim3  dimGrid( gridSizeX, gridSizeY, 1 );
                        //     // print_device_array <<< dimGrid, dimBlock >>> (d_W, level_width, level_height , level );
                        //
                        //     std::cout << A << std::endl;
                        //     std::cout << b << std::endl;
                        // }
                        }
                }

                unbind_textures();  // leave texture references free for binding at level below
        }

        // swap the pointers so we place image in the correct buffer next time this function is called
        temp_swap = d_cur; d_cur = d_prev; d_prev = temp_swap;

        // accumulate total_xi: total_xi = log(exp(xi)*exp(total_xi))
        xi_total = lieLog(lieExp(xi_total)*lieExp(xi).inverse());
        // xi_total = lieLog(lieExp(xi)*lieExp(xi_total));
        return xi_total;
}




private:
// structure saving image data for each level of a pyramid: gray & depth images, and derivatives of gray
struct PyramidLevel { float *gray, *depth, *gray_dx, *gray_dy; };
// host parameters
SolvingMethod solvingMethod;   // enum type of possible solving methods
int maxIterationsPerLevel;
int maxLevel;
int minLevel;   // For speed. Used if the highest precision is not required
int width;   // width of the first frame (and all frames)
int height;   // height of the first frame (and all frames)
// ResidualWeight weightType;   // enum type of possible residual weighting. Defined in *_jacobian.cuh
bool useTDistWeights;  // Whether or not weighting by student t-distribution should be used
Matrix6f A; // A = J' * W * J
Vector6f b; // b = J' * W * r
float error;
float SIGMA_INITIAL = 0.025f;
float VARIANCE_INITIAL = 0.000625f;
float BIG_FLOAT = std::numeric_limits<float>::max(); // TODO: can't this be *too* big?

// cuBLAS variables
#ifdef ENABLE_CUBLAS
        cublasHandle_t handle; // not used if useCUBLAS = false
        cublasStatus_t stat;
#endif
const float alpha = 1.f, beta = 0.f;
static const int NUM_STREAMS = 2;
cudaStream_t streams[NUM_STREAMS];

// device variables
float *d_x_prime; // 3D x position in the second frame
float *d_y_prime; // 3D y position in the second frame
float *d_z_prime; // 3D z position in the second frame
float *d_u_warped;  // warped x position of every pixel in the first image onto the second image
float *d_v_warped;  // warped y position of every pixel in the first image onto the second image
float *d_J;   // device Jacobian array for ALL residuals
float *d_JTW; // device Jacobian transpose * weighting function array
float *d_W;   // device Weight array
float *d_r;   // device residuals array
float *d_b;   // device linear system inhomogeneous term array
float *d_A;   // device linear system matrix array
float *d_error;   // mean squares error of residual
//float *d_sigma;
std::vector<PyramidLevel> d_cur;   // current vector of pointers to device pyramid level structures
std::vector<PyramidLevel> d_prev;   // previous vector of pointers to device pyramid level structures
std::vector<PyramidLevel> temp_swap;
Matrix3f R;
Matrix3f RK_inv;
Vector3f t;
#ifndef ENABLE_CUBLAS
// these shouldn't be declared if CUBLAS is used
float *d_pre_A, *d_pre_A_aux, *d_pre_b, *d_pre_b_aux;
#endif

// host variables
// watch out: Eigen::Matrix are stored column wise
std::vector<Matrix3f> K_pyr;   // stores projection matrix and downsampled version (intrinsic camera properties)
std::vector<Matrix3f> K_inv_pyr;   // inverse K_pyr
Vector6f xi_delta;
Vector6f xi;
Vector6f xi_total;

//______________________________________________________________________________
//______________________________________________________________________________
//_________________PRIVATE FUNCTIONS____________________________________________
//______________________________________________________________________________
//______________________________________________________________________________

//_______________________________________________________
//_______________________________________________________
//________ PREPROCESSING
//_______________________________________________________
//_______________________________________________________

void fill_K_levels(Eigen::Matrix3f K) {
        K_pyr[0] = K;
        K_inv_pyr[0] = invertKMat(K_pyr[0]);
        for (int level = 1; level <= maxLevel; level++) {
                K_pyr[level] = downsampleK(K_pyr[level-1]);
                K_inv_pyr[level] = invertKMat(K_pyr[level]);
        }
}

// watch out: K is stored column-wise !!
void load_K_to_device() {
        float *p_const;
        for (int level = 0; level <= maxLevel; level++) {
                // get the pointer to constant memory
                cudaGetSymbolAddress((void **)&p_const, const_K_pyr); CUDA_CHECK;
                // write each level at an incremented device pointer position
                cudaMemcpy(p_const + 9*level, K_pyr[level].data(), 9*sizeof(float), cudaMemcpyHostToDevice); CUDA_CHECK;
        }
}

/**
 * Copies the input image as the first pyramid level into the device and calculates the remaining levels upwards. Not to be used without access to private variables.
 * @param d_img    Vector of pointers to PyramidLevel structures allocated in device
 * @param grayImg  Gray image of full resolution (first level size) as an array of floats
 * @param depthImg Depth image of full resolution (first level size) as an array of floats
 */
void fill_pyramid(std::vector<PyramidLevel>& d_img, float *grayImg, float *depthImg) {
        // copy image into the basis of the pyramid
        cudaMemcpy(d_img[0].gray, grayImg, width*height*sizeof(float), cudaMemcpyHostToDevice); CUDA_CHECK;
        cudaMemcpy(d_img[0].depth, depthImg, width*height*sizeof(float), cudaMemcpyHostToDevice); CUDA_CHECK;
        //cudaDeviceSynchronize(); // TODO: 2 instances of imresize can't be run in parallel atm, because of some cudaMalloc & cudaFree in that scope
        int level_width, level_height; // width and height of downsampled images
        for (int level = 1; level <= maxLevel; level++) {
                level_width = width / (1 << level); // bitwise operator to divide by 2**level
                level_height = height / (1 << level);
                imresize_CUDA(d_img[level-1].gray, d_img[level].gray, 2*level_width, 2*level_height, level_width, level_height, 1, false/*,0*/); CUDA_CHECK;
                imresize_CUDA(d_img[level-1].depth, d_img[level].depth, 2*level_width, 2*level_height, level_width, level_height, 1, true/*,0*/); CUDA_CHECK; // TODO: Check properly if isDepthImage is working. Looks like it does
        }
        cudaDeviceSynchronize(); // TODO: 2 instances of imresize can't be run in parallel atm, because of some cudaMalloc & cudaFree in that scope
        for (int level = 0; level <= maxLevel; level++) {
                level_width = width / (1 << level); // bitwise operator to divide by 2**level
                level_height = height / (1 << level);
                // compute derivatives!!
                image_derivatives_CUDA(d_img[level].gray,d_img[level].gray_dx,d_img[level].gray_dy,level_width,level_height); CUDA_CHECK;
        }
        // //Debug
        // for (int level = 0; level < maxLevel; level++) {
        //         int level_width = width / (1 << level); // bitwise operator to divide by 2**level
        //         int level_height = height / (1 << level);
        //         cv::Mat mTest(level_height, level_width, CV_32FC1);
        //         float *prev = new float[level_width*level_height];
        //         //DEPTH
        //         cudaMemcpy(prev, d_img[level].depth, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
        //         convert_layered_to_mat(mTest, prev);
        //         double min, max;
        //         cv::minMaxLoc(mTest, &min, &max);
        //         showImage( "Depth: " + std::to_string(level), mTest/max, 100, 100); //cv::waitKey(0); // TODO oskar: .. values are probably not in the correct range.. neither [0,1] nor [0,255].. result is just black n white.. should be gray scale
        //         //GRAY
        //         cudaMemcpy(prev, d_img[level].gray, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
        //         convert_layered_to_mat(mTest, prev);
        //         showImage( "Gray: " + std::to_string(level), mTest, 300, 100); //cv::waitKey(0);
        //         //DERIVATIVES dx
        //         cudaMemcpy(prev, d_img[level].gray_dx, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
        //         convert_layered_to_mat(mTest, prev);
        //         showImage( "DX: " + std::to_string(level), mTest, 300, 100); //cv::waitKey(0);
        //         //DERIVATIVES dx
        //         cudaMemcpy(prev, d_img[level].gray_dy, level_width*level_height*sizeof(float), cudaMemcpyDeviceToHost);
        //         convert_layered_to_mat(mTest, prev);
        //         showImage( "DY: " + std::to_string(level), mTest, 300, 100); cv::waitKey(0);
        //         //cvDestroyAllWindows();
        // }

}

//_______________________________________________________
//_______________________________________________________
//________ ALIGNMENT
//_______________________________________________________
//_______________________________________________________

/**
 * Calculates the transformed positions on the second frame for every pixel in the first
 */
void transform_points(int level, int level_width, int level_height) {
          // Block = 2D array of threads
          dim3  dimBlock( g_CUDA_blockSize2DX, g_CUDA_blockSize2DY, 1 );

          // Grid = 2D array of blocks
            // gridSizeX = ceil( width / nBlocksX )
          int   gridSizeX = (level_width  + dimBlock.x-1) / dimBlock.x;
            // gridSizeY = ceil( height / nBlocksX )
          int   gridSizeY = (level_height + dimBlock.y-1) / dimBlock.y;
          dim3  dimGrid( gridSizeX, gridSizeY, 1 );

          d_transform_points <<< dimGrid, dimBlock >>> (d_x_prime, d_y_prime, d_z_prime, d_u_warped, d_v_warped, d_prev[level].depth, level_width, level_height, level); CUDA_CHECK;
}

/**
 * Calculates the jacobian at each pixel
 */
void calculate_jacobian(int level, int level_width, int level_height, cudaStream_t stream=0) {
          // Block = 2D array of threads
          dim3  dimBlock( g_CUDA_blockSize2DX, g_CUDA_blockSize2DY, 1 );

          // Grid = 2D array of blocks
            // gridSizeX = ceil( width / nBlocksX )
          int   gridSizeX = (level_width  + dimBlock.x-1) / dimBlock.x;
            // gridSizeY = ceil( height / nBlocksX )
          int   gridSizeY = (level_height + dimBlock.y-1) / dimBlock.y;
          dim3  dimGrid( gridSizeX, gridSizeY, 1 );

          d_calculate_jacobian <<< dimGrid, dimBlock, 0, 0 >>> (d_J, d_x_prime, d_y_prime, d_z_prime, d_u_warped, d_v_warped, level_width, level_height, level); // texture is accessed directly. No argument needed
        //   CUDA_CHECK;
}

/**
 * Calculates the residual at each pixel
 */
void calculate_residuals(int level, int level_width, int level_height, cudaStream_t stream=0) {
          // Block = 2D array of threads
          dim3  dimBlock( g_CUDA_blockSize2DX, g_CUDA_blockSize2DY, 1 );

          // Grid = 2D array of blocks
            // gridSizeX = ceil( width / nBlocksX )
          int   gridSizeX = (level_width  + dimBlock.x-1) / dimBlock.x;
            // gridSizeY = ceil( height / nBlocksX )
          int   gridSizeY = (level_height + dimBlock.y-1) / dimBlock.y;
          dim3  dimGrid( gridSizeX, gridSizeY, 1 );

          d_calculate_residuals <<< dimGrid, dimBlock, 0, 0 >>> (d_r, d_prev[level].gray, d_u_warped, d_v_warped, level_width, level_height, level); // texture is accessed directly. No argument needed
        //   CUDA_CHECK;
}

/**
 * Calculates the error
 */
void calculate_error(int level, int level_width, int level_height, cudaStream_t stream=0) {
#ifdef ENABLE_CUBLAS
        int n = level_width*level_height;
        // Calculate the error from the residuals. sum(errors) = r' * r
        cublasSetStream(handle, 0);
        cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, 1, 1, n, &alpha, d_r, n, d_r, n, &beta, d_error, 1);
#else
        int size = level_width * level_height;
        // threads per block equals maximum possible
        int blocklength = 1024;
        // number of needed blocsk is ceil(l_w * l_h / blocklength)
        int nblocks = (size + blocklength -1)/blocklength;

        // alloc auxiliar array
        float *d_aux = NULL;
        cudaMalloc(&d_aux, nblocks*sizeof(float));// CUDA_CHECK;  // to avoid overwriting the residuals array
        // alloc pointer for swapping
        float *d_swap;

        dim3 block = dim3(blocklength,1,1);
        dim3 grid = dim3(nblocks, 1, 1 );

        // first reduction. Return reduction by 1024 in aux
        d_squares_sum <<< grid, block, blocklength*sizeof(float), 0 >>> (d_r, d_aux, size);// CUDA_CHECK;

        // now aux is the input, and size is its size
        size = nblocks;
        // nblocks is the output of the next reduction
        nblocks = (size + blocklength -1)/blocklength;
        grid = dim3(nblocks, 1, 1 );

        // alloc another auxiliar array
        float *d_aux2 = NULL;
        cudaMalloc(&d_aux2, nblocks*sizeof(float));// CUDA_CHECK;  // to avoid overwriting the residuals array

        // reductions until size 1
        while (true) {
                d_sum <<< grid, block, blocklength*sizeof(float), 0 >>> (d_aux, d_aux2, size);// CUDA_CHECK;
                // if no more reductions are needed, break. Result is in d_aux2
                if (nblocks == 1) break;

                // // copy d_aux2 to the beginning of d_aux
                // cudaMemcpy( d_aux, d_aux2, nblocks*sizeof(float), cudaMemcpyDeviceToDevice ); CUDA_CHECK;
                // swap pointers of aux and aux2
                d_swap = d_aux; d_aux = d_aux2; d_aux2 = d_swap;
                // now aux2 is the input, copied into d_aux, and size is its size
                size = nblocks;
                // nblocks is the output of the next reduction
                nblocks = (size + blocklength -1)/blocklength;
                grid = dim3(nblocks, 1, 1 );
        }

        // d_check_error <<< dimGrid, dimBlock, 0, stream >>> (d_error, d_error_prev, d_error_ratio, d_r, level_width, level_height, level);
        cudaMemcpy(d_error, d_aux2, sizeof(float), cudaMemcpyDeviceToDevice); CUDA_CHECK;

        cudaFree(d_aux); cudaFree(d_aux2);

        // // DEBUG
        // float test;
        // cudaMemcpy( &test, d_error, sizeof(float), cudaMemcpyDeviceToHost );
        // std::cout << "elegant: " << test << std::endl;
#endif
}

/**
 * Calculates the weights
 */
void calculate_weights( int level,
                        int level_width,
                        int level_height,
                        float &variance_init,
                        bool useTDist,
                        cudaStream_t stream=0) {
        // Block = 2D array of threads
        dim3  dimBlock( g_CUDA_blockSize2DX, g_CUDA_blockSize2DY, 1 );

        // Grid = 2D array of blocks
            // gridSizeX = ceil( width / nBlocksX )
        int   gridSizeX = (level_width  + dimBlock.x-1) / dimBlock.x;
            // gridSizeY = ceil( height / nBlocksX )
        int   gridSizeY = (level_height + dimBlock.y-1) / dimBlock.y;
        dim3  dimGrid( gridSizeX, gridSizeY, 1 );

        if ( ! useTDist ) {    // use uniform weights
                d_set_uniform_weights <<< dimGrid, dimBlock, 0, 0 >>> (d_W, level_width, level_height); //CUDA_CHECK;
        } else {    // use T-Distribution weights
                int   n = level_width * level_height;

                float variance = variance_init;
                //float sigma = sigma_init;
                int iterations = 0;
                do {
                      variance_init = variance;
                      // here d_W is used just as an auxiliar variable
                      // this returns the squared weighed residuals in d_W, which have to be reduced to get the variance
                      d_calculate_tdist_variance <<< dimGrid, dimBlock, 0, 0 >>> (d_W, d_r, level_width, level_height, variance_init); CUDA_CHECK;

                      //compute new variance:
#ifdef ENABLE_CUBLAS
                      cublasSasum(handle, n , d_W, 1 , &variance);
#else
                      reduce_array_GPU( &variance, d_W, n );
#endif
                      variance /= n;
                      iterations ++;
                } while( (std::abs( 1/(variance) -  1/(variance_init) ) > 1e-3)
                      && (iterations < 5) );

                variance_init = variance;
                d_calculate_tdist_weights <<< dimGrid, dimBlock, 0, 0 >>> (d_W, d_r, level_width, level_height, variance); CUDA_CHECK;

                //cout << "Tdist estimate scale in  " << iterations << " iterations" << endl;

        }
}

// calculate weights from current residuals and update d_jtw
// only used with cuBLAS and only if weights are used
void calculate_jtw(int level, int level_width, int level_height) {
        // calculate_weights( level, level_width, level_height, true );

        int   lev_size = level_width * level_height;

        dim3 dimBlockJ(g_CUDA_blockSize2DX, 6,1);
        int gridSizeX = (lev_size + dimBlockJ.x-1) / dimBlockJ.x;
        dim3 dimGridJ( gridSizeX, 1, 1 );
        cudaDeviceSynchronize(); //streams[0] and streams[1] must be done before the next call
        d_calculate_jtw <<<dimGridJ, dimBlockJ, 0, 0 >>>(d_JTW, d_J, d_W, lev_size, 6); CUDA_CHECK;
        cudaDeviceSynchronize(); // d_JTW must be done before any other calculations on GPU
}

/**
 * Reduce an array.
 * @param p_out  Output. Pointer to a single float in CPU memory.
 * @param d_arr  Input. Pointer to an array of size size in device memory.
 * @param size   Size of d_arr.
 * @param stream CUDA stream to run this if provided.
 */
void reduce_array_GPU ( float *p_out, float *d_arr, int size, cudaStream_t stream=0 ) {
        // threads per block equals maximum possible
        int blocklength = 1024;
        // number of needed blocsk is ceil(l_w * l_h / blocklength)
        int nblocks = (size + blocklength -1)/blocklength;

        // alloc auxiliar array
        float *d_aux = NULL;
        cudaMalloc(&d_aux, nblocks*sizeof(float));// CUDA_CHECK;  // to avoid overwriting the residuals array
        // alloc pointer for swapping
        float *d_swap;

        dim3 block = dim3(blocklength,1,1);
        dim3 grid = dim3(nblocks, 1, 1 );

        // first reduction. Return reduction by 1024 in aux
        // d_sum (input, output, size)
        d_sum <<< grid, block, blocklength*sizeof(float), 0 >>> (d_arr, d_aux, size);// CUDA_CHECK;

        // now aux is the input, and size is its size
        size = nblocks;
        // nblocks is the output of the next reduction
        nblocks = (size + blocklength -1)/blocklength;
        grid = dim3(nblocks, 1, 1 );

        // alloc another auxiliar array
        float *d_aux2 = NULL;
        cudaMalloc(&d_aux2, nblocks*sizeof(float));// CUDA_CHECK;  // to avoid changing the pointer of the original array

        // reductions until size 1
        while (true) {
                // d_sum (input, output, size)
                d_sum <<< grid, block, blocklength*sizeof(float), 0 >>> (d_aux, d_aux2, size);// CUDA_CHECK;
                // if no more reductions are needed, break. Result is in d_aux2
                if (nblocks == 1) break;

                // // copy d_aux2 to the beginning of d_aux
                // cudaMemcpy( d_aux, d_aux2, nblocks*sizeof(float), cudaMemcpyDeviceToDevice ); CUDA_CHECK;
                // swap pointers of aux and aux2
                d_swap = d_aux; d_aux = d_aux2; d_aux2 = d_swap;
                // now aux2 is the input, copied into d_aux, and size is its size
                size = nblocks;
                // nblocks is the output of the next reduction
                nblocks = (size + blocklength -1)/blocklength;
                grid = dim3(nblocks, 1, 1 );
        }

        // d_check_error <<< dimGrid, dimBlock, 0, stream >>> (d_error, d_error_prev, d_error_ratio, d_r, level_width, level_height, level);
        cudaMemcpy( p_out, d_aux2, sizeof(float), cudaMemcpyDeviceToHost); CUDA_CHECK;

        cudaFree(d_aux); cudaFree(d_aux2);

        // // DEBUG
        // float test;
        // cudaMemcpy( &test, d_error, sizeof(float), cudaMemcpyDeviceToHost );
        // std::cout << "elegant: " << test << std::endl;
}

/**
 * Calculate matrix A of the linear system
 */
void calculate_A (int level, int level_width, int level_height, cudaStream_t stream=0) {
#ifdef ENABLE_CUBLAS
        int n = level_width*level_height;
        cublasSetStream(handle, 0);

        // Use nVidia's cuBLAS library for linear algebra calculations

        // Matrix multiplication using cuBLAS looks like this:
        //      C = alpha * D * F + beta * C,    where C,D,F are matrices and alpha & beta are scalars
        //
        // For specifying whether or not the matrices D & F are transposed we use these constants:
        //      CUBLAS_OP_N => non-transpose,
        //      CUBLAS_OP_T => transpose,
        //      CUBLAS_OP_C => transpose conjugate

        // We want to calculate A = J' * W * J : (6x6) matrix
        //      W = IdentityMatrix => A = J' * J : (6x6) matrix
        //          using cuBLAS: A = alpha * J' * J + beta * A
        if(!useTDistWeights) {
                stat = cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, 6, 6, n, &alpha, d_J, n, d_J, n, &beta, d_A, 6);
        } else {
                // JTW = J' * W, that is the transpose of the jacobian multiplied by the diagonal matrix W, storing weights calculated using the residuals
                calculate_jtw(level, level_width, level_height);
                stat = cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, 6, 6, n, &alpha, d_JTW, n, d_J, n, &beta, d_A, 6);
                // std::cout << "We are using weights!!" << std::endl;
        }

        if (stat != CUBLAS_STATUS_SUCCESS) {
                printf ("\n\n!----------cuBLAS matrix multiplication: A = J'*J FAILED!----------!\n\n");
                // return EXIT_FAILURE;
        }
        else {
                //printf ("\n\n!----------cuBLAS matrix multiplication: A = J'*J SUCCESSFUL!----------!\n\n");
                // cudaMemcpy(A.data(), d_A, 6*6*sizeof(float), cudaMemcpyDeviceToHost); CUDA_CHECK;
                // TODO now done in the iteration loop
        }

        // copy A to CPU memory
        cudaDeviceSynchronize();
        cudaMemcpy ( A.data(), d_A, 6*6*sizeof(float), cudaMemcpyDeviceToHost);
        //std::cout << "Matrix A: \n" << A << std::endl;
#else
        int size = level_width * level_height;
        // threads per block equals maximum possible
        int blocklength = 1024;
        // number of needed blocks in 3D
            // size of A is 6x6
        int numblocksX = 6;
        int numblocksY = 6;
            // number of subproducts for each element of A = ceil(size/blocklength)
        int numblocksZ = (size + blocklength -1)/blocklength;
            // total
        int gridVolume = numblocksX*numblocksY*numblocksZ;

        // // alloc auxiliar array for all the matrix sub-products forming the 3D "volume" (stored in an array) previous to computing A
        // float *d_pre_A = NULL;
        // cudaMalloc(&d_pre_A, gridVolume*sizeof(float)); CUDA_CHECK;  // to avoid overwriting the residuals array
        float *d_swap;

        dim3 block = dim3(blocklength,1,1);
        dim3 grid = dim3( numblocksX, numblocksY, numblocksZ );

        // J'*W*J pre-calculation, yet to be reduced. Gets stored into d_pre_A (previous to A)
        cudaDeviceSynchronize(); //Both streams[0] and streams[1] must be 0 before next call
        d_product_JacT_W_Jac <<< grid, block, blocklength*sizeof(float), 0 >>> (d_pre_A, d_J, d_W, size); CUDA_CHECK;

        // now d_pre_A is the input, and size is its size per column to be reduced
        size = numblocksZ;
        // numblocksZ is now the size after the next reduction
        numblocksZ = (size + blocklength -1)/blocklength;

        // // another auxiliar array is needed for storing intermediate results.
        // // This allocates the space needed for the first iteration. There will be
        // // storage in excess for the rest of needed iterations
        // float *d_pre_A_aux = NULL;
        // cudaMalloc(&d_pre_A_aux, numblocksX*numblocksY*numblocksZ*sizeof(float)); CUDA_CHECK;  // to avoid overwriting the residuals array

        while (true) {
                // d_A is the output of the next reduction, is 6x6xnumblocksZ
                grid = dim3( numblocksX, numblocksY, numblocksZ );

                // // if this is the last iteration, write to A
                // if (numblocksZ == 1) d_pre_A_aux = d_A;
                d_reduce_pre_M_towards_M <<< grid, block, blocklength*sizeof(float), 0 >>> (d_pre_A_aux, d_pre_A, size); CUDA_CHECK;

                // swap pre_A and pre_A_aux pointers to change input and output for next iteration
                d_swap = d_pre_A; d_pre_A = d_pre_A_aux; d_pre_A_aux = d_swap;

                // if reduction is complete, break
                if (numblocksZ == 1) break;

                // now d_pre_A has the size d_pre_A_aux had in the previous iteration
                size = numblocksZ;
                // numblocksZ will be the size after the next reduction
                numblocksZ = (size + blocklength -1)/blocklength;
        }

        // copy A to CPU memory
        cudaDeviceSynchronize();
        cudaMemcpy ( A.data(), d_pre_A, 6*6*sizeof(float), cudaMemcpyDeviceToHost);
#endif
}

/**
 * Calculate array b of the linear system.
 *
 * calculate_b is just a particular case of calculate_A. Particularly true in the non cuBLAS version.
 * This is also commented in d_product_JacT_W_res in alignment.cuh
 * No speed improvement at runtime is to be expected from changing this, whereas
 * it could make the code less readable. Thus these operations are executed by
 * different functions and different kernels.
 */
void calculate_b (int level, int level_width, int level_height, cudaStream_t stream=0) {
#ifdef ENABLE_CUBLAS
        int n = level_width*level_height;
        cublasSetStream(handle, 0);
        // Use nVidia's cuBLAS library for linear algebra calculations

        // Matrix multiplication using cuBLAS looks like this:
        //      C = alpha * D * F + beta * C,    where C,D,F are matrices and alpha & beta are scalars
        //
        // For specifying whether or not the matrices D & F are transposed we use these constants:
        //      CUBLAS_OP_N => non-transpose,
        //      CUBLAS_OP_T => transpose,
        //      CUBLAS_OP_C => transpose conjugate

        // We want to calculate b = J' * W * r : (6x1) vector
        //      W = IdentityMatrix => b = J' * r : (6x1) vector
        //          using cuBLAS: b = alpha * J' * r + beta * b
        if(!useTDistWeights)
                stat = cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, 6, 1, n, &alpha, d_J, n, d_r, n, &beta, d_b, 6);
        else
                stat = cublasSgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, 6, 1, n, &alpha, d_JTW, n, d_r, n, &beta, d_b, 6);

        if (stat != CUBLAS_STATUS_SUCCESS) {
                printf ("\n\n!----------cuBLAS matrix multiplication: b = J'*r FAILED!----------!\n\n");
                // return EXIT_FAILURE;
        }
        else {
                // cudaMemcpy(b.data(), d_b, 6*sizeof(float), cudaMemcpyDeviceToHost); CUDA_CHECK;
                // TODO now done in the iteration loop
        }

        // copy b to CPU memory
        cudaDeviceSynchronize();
        cudaMemcpy ( b.data(), d_b, 6*sizeof(float), cudaMemcpyDeviceToHost);
#else
        int size = level_width * level_height;
        // threads per block equals maximum possible
        int blocklength = 1024;
        // number of needed blocks in 3D. Now it is actually 2D becaus numblocksY=1, but it keeps the structure of calculate_A, so it is 3D.
            // size of b is 6x1
        int numblocksX = 6;
        int numblocksY = 1;
            // number of subproducts for each element of b = ceil(size/blocklength)
        int numblocksZ = (size + blocklength -1)/blocklength;
            // total
        int gridVolume = numblocksX*numblocksY*numblocksZ;

        // // alloc auxiliar array for all the matrix sub-products forming the 3D "volume" (stored in an array) previous to computing A
        // float *d_pre_b = NULL;
        // cudaMalloc(&d_pre_b, gridVolume*sizeof(float)); CUDA_CHECK;  // to avoid overwriting the residuals array
        float *d_swap;

        dim3 block = dim3(blocklength,1,1);
        dim3 grid = dim3( numblocksX, numblocksY, numblocksZ );

        // J'*W*J pre-calculation, yet to be reduced. Gets stored into d_pre_b (previous to A)
        d_product_JacT_W_res <<< grid, block, blocklength*sizeof(float), 0 >>> (d_pre_b, d_J, d_W, d_r, size); CUDA_CHECK;

        // now aux is the input, and size is its size
        size = numblocksZ;
        // numblocksZ is now the size after the next reduction
        numblocksZ = (size + blocklength -1)/blocklength;

        // // another auxiliar array is needed for storing intermediate results.
        // // This allocates the space needed for the first iteration. There will be
        // // storage in excess for the rest of needed iterations
        // float *d_pre_b_aux = NULL;
        // cudaMalloc(&d_pre_b_aux, numblocksX*numblocksY*numblocksZ*sizeof(float)); CUDA_CHECK;  // to avoid overwriting the residuals array

        while (true) {
                // d_A is the output of the next reduction, is 6x6xnumblocksZ
                grid = dim3( numblocksX, numblocksY, numblocksZ );

                // if this is the last iteration, write to A
                if (numblocksZ == 1) d_pre_b_aux = d_b;
                d_reduce_pre_M_towards_M <<< grid, block, blocklength*sizeof(float), 0 >>> (d_pre_b_aux, d_pre_b, size); CUDA_CHECK;

                // swap pre_A and pre_A_aux pointers to change input and output for next iteration
                d_swap = d_pre_b; d_pre_b = d_pre_b_aux; d_pre_b_aux = d_swap;

                // if reduction is complete, break
                if (numblocksZ == 1) break;

                // now d_pre_A has the size d_pre_A_aux had in the previous iteration
                size = numblocksZ;
                // numblocksZ will be the size after the next reduction
                numblocksZ = (size + blocklength -1)/blocklength;
        }

        // copy b to CPU memory
        cudaDeviceSynchronize();
        cudaMemcpy ( b.data(), d_pre_b, 6*sizeof(float), cudaMemcpyDeviceToHost);
#endif
}

//_______________________________________________________
//_______________________________________________________
//__________ DEVICE MEMORY allocation & binding
//_______________________________________________________
//_______________________________________________________

void define_texture_parameters() {
        texRef_grayImg.normalized = false; CUDA_CHECK;
        texRef_grayImg.filterMode = cudaFilterModeLinear; CUDA_CHECK;

        texRef_gray_dx.normalized = false; CUDA_CHECK;
        texRef_gray_dx.filterMode = cudaFilterModeLinear; CUDA_CHECK;

        texRef_gray_dy.normalized = false; CUDA_CHECK;
        texRef_gray_dy.filterMode = cudaFilterModeLinear; CUDA_CHECK;
}

void bind_textures(int level, int level_width, int level_height) {
        cudaChannelFormatDesc desc = cudaCreateChannelDesc<float>(); // number of bits for each texture
        int pitch = level_width * sizeof(float);
        cudaBindTexture2D(NULL, &texRef_grayImg, d_cur[level].gray, &desc, level_width, level_height, pitch); CUDA_CHECK;
        cudaBindTexture2D(NULL, &texRef_gray_dx, d_cur[level].gray_dx, &desc, level_width, level_height, pitch); CUDA_CHECK;
        cudaBindTexture2D(NULL, &texRef_gray_dy, d_cur[level].gray_dy, &desc, level_width, level_height, pitch); CUDA_CHECK;
}

void unbind_textures() {
        cudaUnbindTexture(texRef_grayImg); CUDA_CHECK;
        cudaUnbindTexture(texRef_gray_dx); CUDA_CHECK;
        cudaUnbindTexture(texRef_gray_dy); CUDA_CHECK;
}

void allocateGPUMemory() {
        cudaMalloc(&d_J,      6*width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_JTW,    6*width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_W,        width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_r,        width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_x_prime,  width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_y_prime,  width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_z_prime,  width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_u_warped, width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_v_warped, width*height*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_b,                   6*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_A,                 6*6*sizeof(float)); CUDA_CHECK;
        cudaMalloc(&d_error,                 sizeof(float)); CUDA_CHECK;
        //cudaMalloc(&d_sigma,                 sizeof(float)); CUDA_CHECK;
        // cudaMalloc(&d_visualResidual, width*height*sizeof(float)); CUDA_CHECK;
        // cudaMalloc(&d_n, sizeof(int)); CUDA_CHECK;

        // allocate pyramid vector levels in device memory
        for (int level = 0; level <= maxLevel; level++) {
                int level_width = width / (1 << level); // calculating bitwise the succesive powers of 2
                int level_height = height / (1 << level);
                cudaMalloc(&d_cur [level].gray,    level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_prev[level].gray,    level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_cur [level].depth,   level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_prev[level].depth,   level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_cur [level].gray_dx, level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_prev[level].gray_dx, level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_cur [level].gray_dy, level_width*level_height*sizeof(float)); CUDA_CHECK;
                cudaMalloc(&d_prev[level].gray_dy, level_width*level_height*sizeof(float)); CUDA_CHECK;
        }

#ifndef ENABLE_CUBLAS
        // these shouldn't be declared if CUBLAS is used
        // auxiliar arrays for cuda matrix multiplications
        cudaMalloc(&d_pre_A, (6*6*width*height*sizeof(float) + 1023)/1024); CUDA_CHECK;  // to avoid overwriting the residuals array
        cudaMalloc(&d_pre_A_aux, (6*6*width*height*sizeof(float) + 1023)/1024); CUDA_CHECK;  // to avoid overwriting the residuals array
        cudaMalloc(&d_pre_b, (6*1*width*height*sizeof(float) + 1023)/1024); CUDA_CHECK;  // to avoid overwriting the residuals array
        cudaMalloc(&d_pre_b_aux, (6*1*width*height*sizeof(float) + 1023)/1024); CUDA_CHECK;  // to avoid overwriting the residuals array
#endif

}

void deallocateGPUMemory() {
        cudaFree(d_J);        CUDA_CHECK;
        cudaFree(d_JTW);      CUDA_CHECK;
        cudaFree(d_W);        CUDA_CHECK;
        cudaFree(d_r);        CUDA_CHECK;
        cudaFree(d_x_prime);  CUDA_CHECK;
        cudaFree(d_y_prime);  CUDA_CHECK;
        cudaFree(d_z_prime);  CUDA_CHECK;
        cudaFree(d_u_warped); CUDA_CHECK;
        cudaFree(d_v_warped); CUDA_CHECK;
        cudaFree(d_b);        CUDA_CHECK;
        cudaFree(d_A);        CUDA_CHECK;
        cudaFree(d_error);    CUDA_CHECK;
        //cudaFree(d_sigma);    CUDA_CHECK;
        // cudaFree(d_visualResidual); CUDA_CHECK;
        // cudaFree(d_n); CUDA_CHECK;

        for (int level = 0; level <= maxLevel; level++) {
                cudaFree(d_cur [level].gray); CUDA_CHECK;
                cudaFree(d_prev[level].gray); CUDA_CHECK;
                cudaFree(d_cur [level].depth); CUDA_CHECK;
                cudaFree(d_prev[level].depth); CUDA_CHECK;
                cudaFree(d_cur [level].gray_dx); CUDA_CHECK;
                cudaFree(d_prev[level].gray_dx); CUDA_CHECK;
                cudaFree(d_cur [level].gray_dy); CUDA_CHECK;
                cudaFree(d_prev[level].gray_dy); CUDA_CHECK;
        }


}

};
