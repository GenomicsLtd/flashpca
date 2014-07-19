
#include "randompca.hpp"
#include "util.hpp"

MatrixXd make_gaussian(unsigned int rows, unsigned int cols, long seed)
{
   boost::random::mt19937 rng;
   rng.seed(seed);
   boost::random::normal_distribution<double> nrm;
   boost::random::variate_generator<boost::random::mt19937&,
      boost::random::normal_distribution<double> > rand(rng, nrm);

   MatrixXd G(rows, cols);
   for(unsigned int i = 0 ; i < rows ; i++)
      for(unsigned int j = 0 ; j < cols ; j++)
	 G(i, j) = rand();
   return G;
}

// normalize each column of X to unit l2 norm
inline void normalize(MatrixXd& X)
{
   unsigned int p = X.cols();
   for(unsigned int j = 0 ; j < p ; j++)
   {
      double s = 1 / sqrt(X.col(j).array().pow(2).sum());
      X.col(j) = X.col(j).array() * s;
   }
}

void pca_small(MatrixXd &B, int method, MatrixXd& U, VectorXd &d, bool verbose)
{
   if(method == METHOD_SVD)
   {
      verbose && std::cout << timestamp() << " SVD begin" << std::endl;
      JacobiSVD<MatrixXd> svd(B, ComputeThinU | ComputeThinV);
      U = svd.matrixU();
      MatrixXd V = svd.matrixV();
      d = svd.singularValues().array().pow(2);
      verbose && std::cout << timestamp() << " SVD done" << std::endl;
   }
   else if(method == METHOD_EIGEN)
   {
      verbose && std::cout << timestamp() << " Eigen-decomposition begin" << std::endl;
      MatrixXd BBT = B * B.transpose();
      verbose && std::cout << timestamp() << " dim(BBT): " << dim(BBT) << std::endl;
      SelfAdjointEigenSolver<MatrixXd> eig(BBT);

      // The eigenvalues come out sorted in *increasing* order,
      // but we need decreasing order
      VectorXd eval = eig.eigenvalues();
      MatrixXd evec = eig.eigenvectors();
      d.resize(eval.size());
      U.resize(BBT.rows(), BBT.rows());

      unsigned int k = 0, s = d.size();
      for(unsigned int i = d.size() - 1 ; i != -1 ; --i)
      {
	 // we get eigenvalues, which are the squared singular values
	 d(k) = eval(i);
	 U.col(k) = evec.col(i);
	 k++;
      }
   }
}

// Compute median of pairwise distances on sample of size n from the matrix X
// We're sampling with replacement
// Based on http://www.machinedlearnings.com/2013/08/cosplay.html
double median_dist(MatrixXd& X, unsigned int n, long seed, bool verbose)
{
   boost::random::mt19937 rng;
   rng.seed(seed);
   boost::random::uniform_real_distribution<> dist(0, 1);
   double prop = (double)X.rows() / n;

   verbose && std::cout << timestamp() << 
      " Computing median Euclidean distance (" << n << " samples)" <<
      std::endl;

   MatrixXd X2(n, X.cols());
   if(n < X.rows()) 
   {
      verbose && std::cout << timestamp() << " Sampling" << std::endl;
      // Sample n rows from X
      for(unsigned int i = 0, k = 0 ; i < X.rows() ; i++)
      {
         if(dist(rng) < prop)
         {
            X2.row(k++) = X.row(i);
            if(k == n)
               break;
         }
      }
   }
   else
      X2.noalias() = X;

   VectorXd norms = X2.array().square().rowwise().sum();
   VectorXd ones = VectorXd::Ones(n);
   MatrixXd R = norms * ones.transpose();
   MatrixXd D = R + R.transpose() - 2 * X2 * X2.transpose();

   unsigned int m = D.size();
   double *d = D.data();
   std::sort(d, d + m); 
   double med;

   if(m % 2 == 0)
      med = (d[m / 2 - 1] + d[m / 2]) / 2;
   else
      med = d[m / 2];

   verbose && std::cout << timestamp() << " Median Euclidean distance: "
      << med << std::endl;

   return med;
}

MatrixXd rbf_kernel(MatrixXd& X, const double sigma, bool rbf_center,
   bool verbose)
{
   unsigned int n = X.rows();
   VectorXd norms = X.array().square().rowwise().sum();
   VectorXd ones = VectorXd::Ones(n);
   MatrixXd R = norms * ones.transpose();
   MatrixXd D = R + R.transpose() - 2 * X * X.transpose();
   D = D.array() / (-1 * sigma * sigma);
   MatrixXd K = D.array().exp();

   if(rbf_center)
   {
      verbose && std::cout << timestamp() << " Centering RBF kernel" << std::endl;
      MatrixXd M = ones * ones.transpose() / n;
      MatrixXd I = ones.asDiagonal();
      K = (I - M) * K * (I - M);
   }
   return K;
}

void RandomPCA::pca(MatrixXd &X, int method, bool transpose,
   unsigned int ndim, unsigned int nextra, unsigned int maxiter, double tol,
   long seed, int kernel, double sigma, bool rbf_center,
   unsigned int rbf_sample, bool save_kernel, bool do_orth, bool do_loadings)
{
   unsigned int N;

   if(kernel != KERNEL_LINEAR)
   {
      transpose = false;
      verbose && std::cout << timestamp()
	 << " Kernel not linear, can't transpose" << std::endl;
   }

   verbose && std::cout << timestamp() << " Transpose: " 
      << (transpose ? "yes" : "no") << std::endl;

   if(transpose)
   {
      if(stand_method != STANDARDIZE_NONE)
	  X_meansd = standardize_transpose(X, stand_method, verbose);
      N = X.cols();
   }
   else
   {
      if(stand_method != STANDARDIZE_NONE)
	 X_meansd = standardize(X, stand_method, verbose);
      N = X.rows();
   }

   unsigned int total_dim = ndim + nextra;
   MatrixXd R = make_gaussian(X.cols(), total_dim, seed);
   MatrixXd Y = X * R;
   verbose && std::cout << timestamp() << " dim(Y): " << dim(Y) << std::endl;
   normalize(Y);
   MatrixXd Yn;

   verbose && std::cout << timestamp() << " dim(X): " << dim(X) << std::endl;
   MatrixXd K; 
   if(kernel == KERNEL_RBF)
   {
      if(sigma == 0)
      {
	 unsigned int med_samples = fminl(rbf_sample, N);
      	 double med = median_dist(X, med_samples, seed, verbose);
      	 sigma = sqrt(med);
      }
      verbose && std::cout << timestamp() << " Using RBF kernel with sigma="
	 << sigma << std::endl;
      K.noalias() = rbf_kernel(X, sigma, rbf_center, verbose);
   }
   else
   {
      verbose && std::cout << timestamp() << " Using linear kernel" << std::endl;
      K.noalias() = X * X.transpose() / (N - 1);
   }

   //trace = K.diagonal().array().sum() / (N - 1);
   trace = K.diagonal().array().sum();
   verbose && std::cout << timestamp() << " Trace(K): " << trace 
      << " (N: " << N << ")" << std::endl;

   verbose && std::cout << timestamp() << " dim(K): " << dim(K) << std::endl;
   if(save_kernel)
   {
      verbose && std::cout << timestamp() << " saving K" << std::endl;
      save_text("kernel.txt", K);
   }

   for(unsigned int iter = 0 ; iter < maxiter ; iter++)
   {
      verbose && std::cout << timestamp() << " iter " << iter;
      Yn.noalias() = K * Y;
      if(do_orth)
      {
	 verbose && std::cout << " (orthogonalising)";
	 ColPivHouseholderQR<MatrixXd> qr(Yn);
	 MatrixXd I = MatrixXd::Identity(Yn.rows(), Yn.cols());
	 Yn = qr.householderQ() * I;
	 Yn.conservativeResize(NoChange, Yn.cols());
      }
      else
	 normalize(Yn);

      double diff =  (Y -  Yn).array().square().sum() / Y.size(); 
      verbose && std::cout << " " << diff << std::endl;
      Y.noalias() = Yn;
      if(diff < tol)
	 break;
   }

   verbose && std::cout << timestamp() << " QR begin" << std::endl;
   ColPivHouseholderQR<MatrixXd> qr(Y);
   MatrixXd Q = MatrixXd::Identity(Y.rows(), Y.cols());
   Q = qr.householderQ() * Q;
   Q.conservativeResize(NoChange, Y.cols());
   verbose && std::cout << timestamp() << " dim(Q): " << dim(Q) << std::endl;
   verbose && std::cout << timestamp() << " QR done" << std::endl;

   MatrixXd B = Q.transpose() * X;
   verbose && std::cout << timestamp() << " dim(B): " << dim(B) << std::endl;

   MatrixXd Et;
   pca_small(B, method, Et, d, verbose);
   verbose && std::cout << timestamp() << " dim(Et): " << dim(Et) << std::endl;

   d = d.array() / (N - 1);

   if(transpose)
   {
      V.noalias() = Q * Et;
      // We divide P by sqrt(N - 1) since X has not been divided
      // by it (but B has)
      P.noalias() = X.transpose() * V;
      VectorXd s = 1 / (d.array().sqrt() * sqrt(N - 1));
      MatrixXd Dinv = s.asDiagonal();
      U = P * Dinv;
   }
   else
   {
      // P = U D = X V
      U.noalias() = Q * Et;
      P.noalias() = U * d.asDiagonal();
      if(do_loadings)
      {
	 VectorXd s = 1 / (d.array().sqrt() * sqrt(N - 1));
	 MatrixXd Dinv = s.asDiagonal();
	 V = X.transpose() * U * Dinv;
      }
   }

   P.conservativeResize(NoChange, ndim);
   U.conservativeResize(NoChange, ndim);
   V.conservativeResize(NoChange, ndim);
   d.conservativeResize(ndim);
   pve = d.array() / trace;
}

// ZCA of genotypes
void RandomPCA::zca_whiten(bool transpose)
{
   verbose && std::cout << timestamp() << " Whitening begin" << std::endl;
   VectorXd s = 1 / d.array();
   MatrixXd Dinv = s.asDiagonal();

   if(transpose)
      W.noalias() = U * Dinv * U.transpose() * X.transpose();
   else
      W.noalias() = U * Dinv * U.transpose() * X;
   verbose && std::cout << timestamp() << " Whitening done (" << dim(W) << ")" << std::endl;
}

