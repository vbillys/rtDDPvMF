/* Copyright (c) 2015, Julian Straub <jstraub@csail.mit.edu> Licensed
 * under the MIT license. See the license file LICENSE.
 */
#pragma once

#include <rtDDPvMF/root_includes.hpp>

#include <signal.h>
#include <string>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/contrib/contrib.hpp>

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>

#include <Eigen/Dense>

// CUDA runtime
//#include <cuda_runtime.h>

// Utilities and system includes
//#include <helper_functions.h>
//#include <nvidia/helper_cuda.h>
//
//#include <cuda_pc_helpers.h>
//#include <convolutionSeparable_common.h>
//#include <convolutionSeparable_common_small.h>
//#include <cv_helpers.hpp>
//#include <pcl_helpers.hpp>
#include <pcl/impl/point_types.hpp>

//#include <vtkWindowToImageFilter.h>
//#include <vtkPNGWriter.h>


#include <dpMMlowVar/sphericalData.hpp>
#include <dpMMlowVar/kmeansCUDA.hpp>
#include <dpMMlowVar/SO3.hpp>

#include <jsCore/timerLog.hpp>
#include <cudaPcl/openniSmoothNormalsGpu.hpp>

using namespace Eigen;

//#define RM_NANS_FROM_DEPTH
// normals without nans
//TimerLog: stats over timer cycles (mean +- 3*std):  3.99052+- 6.63986 16.312+- 10.869 43.0436+- 19.7957
// full depth image with nans
//TimerLog: stats over timer cycles (mean +- 3*std):  4.58002+- 9.72736 19.0138+- 17.9823 49.9746+- 30.6944


class RealtimeSpkm : public cudaPcl::OpenniSmoothNormalsGpu
{
  public:
    RealtimeSpkm(std::string pathOut, double f_d, double eps, uint32_t B, uint32_t K);
    ~RealtimeSpkm();

    virtual void normals_cb(float* d_normals, uint8_t* haveData, uint32_t w, uint32_t h);

    jsc::TimerLog tLog_;
    double residual_;
    uint32_t nIter_;

    void visualizePc();


  protected:
    static const uint32_t SUBSAMPLE_STEP = 1;

    string resultsPath_;
    ofstream fout_;

    uint32_t K_;
    VectorXu z_;
    MatrixXf centroids_;
//    MatrixXf prevCentroids_;
//    MatrixXf deltaR_;
//    MatrixXf R_;
//    GpuMatrix<float> d_R_;
    cv::Mat zIrgb;// (nDisp_.height/SUBSAMPLE_STEP,nDisp_.width/SUBSAMPLE_STEP,CV_8UC3);
    cv::Mat Icomb;// (nDisp_.height/SUBSAMPLE_STEP,nDisp_.width/SUBSAMPLE_STEP,CV_8UC3);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr centroidsPc_;

//    boost::shared_ptr<MatrixXf> spx_; // normals
    boost::shared_ptr<jsc::ClDataGpuf> cld_; // clustered data
    float lambda_, beta_, Q_;
    boost::mt19937 rndGen_;

    dplv::KMeansCUDA<float,dplv::Spherical<float> >* pspkm_;

    void fillJET();
    float JET_r_[256];
    float JET_g_[256];
    float JET_b_[256];
};
// ---------------------------------- impl -----------------------------------


RealtimeSpkm::RealtimeSpkm(std::string pathOut, double f_d, double eps, uint32_t B, uint32_t K)
  : OpenniSmoothNormalsGpu(f_d, eps, B, true),
    tLog_(pathOut+std::string("./timer.log"),2,5,"TimerLog"),
  residual_(0.0), nIter_(10),
  resultsPath_(pathOut),
  fout_((pathOut+std::string("./stats.log")).data(),ofstream::out),
//  d_R_(3,3),
  rndGen_(91)
{
  fillJET();
  cout<<"inititalizing optSO3"<<endl;
  shared_ptr<MatrixXf> tmp(new MatrixXf(3,1));
  (*tmp) << 1,0,0; // init just to get the dimensions right.
  cld_ = shared_ptr<jsc::ClDataGpuf>(new jsc::ClDataGpuf(tmp,K));

  pspkm_ =  new dplv::KMeansCUDA<float,dplv::Spherical<float> >(cld_);

  centroids_ = MatrixXf::Zero(3,1); centroids_ << 1.,0,0;
//  prevCentroids_ = MatrixXf::Zero(3,0);
//  R_ = MatrixXf::Identity(3,3);
//  d_R_.set(R_);
}

RealtimeSpkm::~RealtimeSpkm()
{
  if(pspkm_) delete pspkm_;
  fout_.close();
}

void RealtimeSpkm::normals_cb(float *d_normals, uint8_t* d_haveData, uint32_t w, uint32_t h)
{
//  cout<<"rotating pc by"<<endl<<d_R_.get()<<endl;
//  rotatePcGPU(d_normals,d_R_.data(),w*h,3);
  tLog_.tic(-1); // reset all timers

//  pspkm_->nextTimeStep(d_normals,w*h,3,0);
  int32_t nComp = 0;
  float* d_nComp = this->normalExtract->d_normalsComp(nComp);
//  cout<<"compressed to "<<nComp<<endl;
  pspkm_->nextTimeStepGpu(d_nComp,nComp,3,0,false);

  tLog_.tic(0);
  for(uint32_t i=0; i<nIter_; ++i)
  {
    cout<<"@"<<i<<" :"<<endl;
    pspkm_->updateLabels();
    pspkm_->updateCenters();
    if(pspkm_->convergedCounts(nComp/100)) break;
  }
    cout<<pspkm_->centroids()<<endl;
  tLog_.toc(0);
  pspkm_->getZfromGpu(); // cache z_ back from gpu
  if(tLog_.startLogging()) pspkm_->dumpStats(fout_);

  {
    boost::mutex::scoped_lock updateLock(this->updateModelMutex);
    if(z_.rows() != w*h) z_.resize(w*h);
    this->normalExtract->uncompressCpu(pspkm_->z().data(),pspkm_->z().rows() ,z_.data(),z_.rows());
    K_ = pspkm_->getK();

    centroids_ = pspkm_->centroids();
//    prevCentroids_ = pspkm_->prevCentroids(); // get them from internal since they keep track of removed clusters
//    std::vector<MatrixXf> Rs(std::min(centroids_.cols(),prevCentroids_.cols()));
//    for(uint32_t k=0; k<centroids_.cols(); ++k)
//      if(k < prevCentroids_.cols())
//      {
//        Rs[k] = rotationFromAtoB<float>(prevCentroids_.col(k),centroids_.col(k));
//      }
//    if(Rs.size()>0)
//    {
//      deltaR_ = SO3<float>::meanRotation(Rs,pspkm_->counts().cast<float>(),20);
//      cout<<deltaR_<<endl;
//      R_ = deltaR_*R_;
//      pspkm_->rotateUninstantiated(deltaR_.transpose());
//    }
  }

  pspkm_->updateState();
  tLog_.toc(1); // total time
  tLog_.logCycle();
  cout<<"---------------------------------------------------------------------------"<<endl;
  tLog_.printStats();
  cout<<" residual="<<residual_<<endl;
  cout<<"---------------------------------------------------------------------------"<<endl;

  {
    boost::mutex::scoped_lock updateLock(updateModelMutex);
    pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr nDispPtr = normalExtract->normalsPc();
    nDisp_ = pcl::PointCloud<pcl::PointXYZRGB>::Ptr( new pcl::PointCloud<pcl::PointXYZRGB>(*nDispPtr));
    normalsImg_ = normalExtract->normalsImg();
    this->update_ = true;
  }
}

void RealtimeSpkm::visualizePc()
{
  //copy again
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr nDisp(
      new pcl::PointCloud<pcl::PointXYZRGB>(*nDisp_));
//  cv::Mat nI(nDisp->height,nDisp->width,CV_32FC3);
//  for(uint32_t i=0; i<nDisp->width; ++i)
//    for(uint32_t j=0; j<nDisp->height; ++j)
//    {
//      // nI is BGR but I want R=x G=y and B=z
//      nI.at<cv::Vec3f>(j,i)[0] = (1.0f+nDisp->points[i+j*nDisp->width].z)*0.5f; // to match pc
//      nI.at<cv::Vec3f>(j,i)[1] = (1.0f+nDisp->points[i+j*nDisp->width].y)*0.5f;
//      nI.at<cv::Vec3f>(j,i)[2] = (1.0f+nDisp->points[i+j*nDisp->width].x)*0.5f;
//      nDisp->points[i+j*nDisp->width].rgb=0;
//    }
//  cv::imshow("normals",nI);
  cv::Mat nI(this->normalsImg_.rows,this->normalsImg_.cols,CV_8UC3);
  cv::Mat nIRGB(this->normalsImg_.rows,this->normalsImg_.cols,CV_8UC3);
  this->normalsImg_.convertTo(nI,CV_8UC3,127.5f,127.5f);
  cv::cvtColor(nI,nIRGB,CV_RGB2BGR);
  cv::imshow("normals",nIRGB);
//  cv::imshow("normals",this->normalsImg_);
//  this->pc_ = pcl::PointCloud<pcl::PointXYZRGB>::Ptr(nDisp);

//  cout<<z_.rows()<<" "<<z_.cols()<<endl;
//  cout<<z_.transpose()<<endl;
//

  uint32_t Kmax = 5;
  uint32_t k=0;
//  cout<<" z shape "<<z_.rows()<<" "<< nDisp->width<<" " <<nDisp->height<<endl;
//  cv::Mat Iz(nDisp->height/SUBSAMPLE_STEP,nDisp->width/SUBSAMPLE_STEP,CV_8UC1);
  zIrgb = cv::Mat(nDisp->height/SUBSAMPLE_STEP,nDisp->width/SUBSAMPLE_STEP,CV_8UC3);
  for(uint32_t i=0; i<nDisp->width; i+=SUBSAMPLE_STEP)
    for(uint32_t j=0; j<nDisp->height; j+=SUBSAMPLE_STEP)
      if(nDisp->points[i+j*nDisp->width].x == nDisp->points[i+j*nDisp->width].x )
      {
#ifdef RM_NANS_FROM_DEPTH
        uint8_t idz = (static_cast<uint8_t>(z_(k)))*255/Kmax;
#else
        uint8_t idz = (static_cast<uint8_t>(z_(nDisp->width*j +i)))*255/Kmax;
#endif
        //            cout<<"k "<<k<<" "<< z_.rows() <<"\t"<<z_(k)<<"\t"<<int32_t(idz)<<endl;
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[0] = JET_b_[idz]*255;
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[1] = JET_g_[idz]*255;
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[2] = JET_r_[idz]*255;
        k++;
      }else{
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[0] = 255;
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[1] = 255;
        zIrgb.at<cv::Vec3b>(j/SUBSAMPLE_STEP,i/SUBSAMPLE_STEP)[2] = 255;
      }

//  cout<<this->rgb_.rows <<" " << this->rgb_.cols<<endl;
  if(this->rgb_.rows>1 && this->rgb_.cols >1)
  {
    cv::addWeighted(this->rgb_ , 0.7, zIrgb, 0.3, 0.0, Icomb);
    cv::imshow("dbg",Icomb);
  }else{
    cv::imshow("dbg",zIrgb);
  }

////  uint32_t k=0, Kmax=10;
  for(uint32_t i=0; i<nDisp->width; i+=SUBSAMPLE_STEP)
    for(uint32_t j=0; j<nDisp->height; j+=SUBSAMPLE_STEP)
//      if(nDisp->points[i+j*nDisp->width].x == nDisp->points[i+j*nDisp->width].x )
      if(z_(nDisp->width*j +i) <= Kmax )
      {
//        if(z_(k) == 4294967295)
//        if(z_(i+j*nDisp->width) == 4294967295)
//          cout<<" Problem"<<endl;

//         k = nDisp->width*j +i;
#ifdef RM_NANS_FROM_DEPTH
        uint8_t idz = (static_cast<uint8_t>(z_(k)))*255/Kmax;
#else
        uint8_t idz = (static_cast<uint8_t>(z_(nDisp->width*j +i)))*255/Kmax;
//        cout << z_(nDisp->width*j +i)<<endl;
#endif
//        if(z_(nDisp->width*j +i)>0)
//        cout<<z_(nDisp->width*j +i)<< " " <<int(idz)<<endl;
//        n->points[k] = nDisp->points[i+j*nDisp->width];
        nDisp->points[nDisp->width*j +i].r = static_cast<uint8_t>(floor(JET_r_[idz]*255));
        nDisp->points[nDisp->width*j +i].g = static_cast<uint8_t>(floor(JET_g_[idz]*255));
        nDisp->points[nDisp->width*j +i].b = static_cast<uint8_t>(floor(JET_b_[idz]*255));
        //              n->push_back(pcl::PointXYZL());
        //              nDisp->points[i].x,nDisp->points[i].y,nDisp->points[i].z,z_(k)));
        k++;
      }else{
        nDisp->points[nDisp->width*j +i].x = 0.0;
        nDisp->points[nDisp->width*j +i].y = 0.0;
        nDisp->points[nDisp->width*j +i].z = 0.0;
        nDisp->points[nDisp->width*j +i].r = 255;
        nDisp->points[nDisp->width*j +i].g = 255;
        nDisp->points[nDisp->width*j +i].b = 255;
      }

#ifdef USE_PCL_VIEWER                                         
  this->pc_ = nDisp;
  if(!this->viewer_->updatePointCloud(pc_, "pc"))
    this->viewer_->addPointCloud(pc_, "pc");

//  if(!updateCosy(this->viewer_,R_,"R"))
//    addCosy(this->viewer_,R_, "R");

//  centPc = pcl::PointCloud<pcl::PointXYZRGB>::Ptr(
//      new pcl::PointCloud<pcl::PointXYZRGB>(K_,1));
//  this->viewer_->removeAllShapes();
  for(uint32_t k=0; k<Kmax; ++k)
  {
    char name[20];
    sprintf(name,"cent%d",k);
    this->viewer_->removeShape(std::string(name));
  }
  for(uint32_t k=0; k<K_; ++k)
  {
    uint8_t idz = (static_cast<uint8_t>(k))*255/Kmax;
    pcl::PointXYZ pt;
    pt.x = centroids_(0,k)*1.2;
    pt.y = centroids_(1,k)*1.2;
    pt.z = centroids_(2,k)*1.2;
    double r = JET_r_[idz];
    double g = JET_g_[idz];
    double b = JET_b_[idz];
    char name[20];
    sprintf(name,"cent%d",k);
    if(!this->viewer_->updateSphere(pt,0.1,r,g,b, std::string(name)))
      this->viewer_->addSphere(pt,0.1,r,g,b, std::string(name));
  }
//  centroidsPc_ = centPc;
#endif

}

void RealtimeSpkm::fillJET()
{
  float JET_r[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.00588235294117645,0.02156862745098032,0.03725490196078418,0.05294117647058827,0.06862745098039214,0.084313725490196,0.1000000000000001,0.115686274509804,0.1313725490196078,0.1470588235294117,0.1627450980392156,0.1784313725490196,0.1941176470588235,0.2098039215686274,0.2254901960784315,0.2411764705882353,0.2568627450980392,0.2725490196078431,0.2882352941176469,0.303921568627451,0.3196078431372549,0.3352941176470587,0.3509803921568628,0.3666666666666667,0.3823529411764706,0.3980392156862744,0.4137254901960783,0.4294117647058824,0.4450980392156862,0.4607843137254901,0.4764705882352942,0.4921568627450981,0.5078431372549019,0.5235294117647058,0.5392156862745097,0.5549019607843135,0.5705882352941174,0.5862745098039217,0.6019607843137256,0.6176470588235294,0.6333333333333333,0.6490196078431372,0.664705882352941,0.6803921568627449,0.6960784313725492,0.7117647058823531,0.7274509803921569,0.7431372549019608,0.7588235294117647,0.7745098039215685,0.7901960784313724,0.8058823529411763,0.8215686274509801,0.8372549019607844,0.8529411764705883,0.8686274509803922,0.884313725490196,0.8999999999999999,0.9156862745098038,0.9313725490196076,0.947058823529412,0.9627450980392158,0.9784313725490197,0.9941176470588236,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0.9862745098039216,0.9705882352941178,0.9549019607843139,0.93921568627451,0.9235294117647062,0.9078431372549018,0.892156862745098,0.8764705882352941,0.8607843137254902,0.8450980392156864,0.8294117647058825,0.8137254901960786,0.7980392156862743,0.7823529411764705,0.7666666666666666,0.7509803921568627,0.7352941176470589,0.719607843137255,0.7039215686274511,0.6882352941176473,0.6725490196078434,0.6568627450980391,0.6411764705882352,0.6254901960784314,0.6098039215686275,0.5941176470588236,0.5784313725490198,0.5627450980392159,0.5470588235294116,0.5313725490196077,0.5156862745098039,0.5};
  float JET_g[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.001960784313725483,0.01764705882352935,0.03333333333333333,0.0490196078431373,0.06470588235294117,0.08039215686274503,0.09607843137254901,0.111764705882353,0.1274509803921569,0.1431372549019607,0.1588235294117647,0.1745098039215687,0.1901960784313725,0.2058823529411764,0.2215686274509804,0.2372549019607844,0.2529411764705882,0.2686274509803921,0.2843137254901961,0.3,0.3156862745098039,0.3313725490196078,0.3470588235294118,0.3627450980392157,0.3784313725490196,0.3941176470588235,0.4098039215686274,0.4254901960784314,0.4411764705882353,0.4568627450980391,0.4725490196078431,0.4882352941176471,0.503921568627451,0.5196078431372548,0.5352941176470587,0.5509803921568628,0.5666666666666667,0.5823529411764705,0.5980392156862746,0.6137254901960785,0.6294117647058823,0.6450980392156862,0.6607843137254901,0.6764705882352942,0.692156862745098,0.7078431372549019,0.723529411764706,0.7392156862745098,0.7549019607843137,0.7705882352941176,0.7862745098039214,0.8019607843137255,0.8176470588235294,0.8333333333333333,0.8490196078431373,0.8647058823529412,0.8803921568627451,0.8960784313725489,0.9117647058823528,0.9274509803921569,0.9431372549019608,0.9588235294117646,0.9745098039215687,0.9901960784313726,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0.9901960784313726,0.9745098039215687,0.9588235294117649,0.943137254901961,0.9274509803921571,0.9117647058823528,0.8960784313725489,0.8803921568627451,0.8647058823529412,0.8490196078431373,0.8333333333333335,0.8176470588235296,0.8019607843137253,0.7862745098039214,0.7705882352941176,0.7549019607843137,0.7392156862745098,0.723529411764706,0.7078431372549021,0.6921568627450982,0.6764705882352944,0.6607843137254901,0.6450980392156862,0.6294117647058823,0.6137254901960785,0.5980392156862746,0.5823529411764707,0.5666666666666669,0.5509803921568626,0.5352941176470587,0.5196078431372548,0.503921568627451,0.4882352941176471,0.4725490196078432,0.4568627450980394,0.4411764705882355,0.4254901960784316,0.4098039215686273,0.3941176470588235,0.3784313725490196,0.3627450980392157,0.3470588235294119,0.331372549019608,0.3156862745098041,0.2999999999999998,0.284313725490196,0.2686274509803921,0.2529411764705882,0.2372549019607844,0.2215686274509805,0.2058823529411766,0.1901960784313728,0.1745098039215689,0.1588235294117646,0.1431372549019607,0.1274509803921569,0.111764705882353,0.09607843137254912,0.08039215686274526,0.06470588235294139,0.04901960784313708,0.03333333333333321,0.01764705882352935,0.001960784313725483,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  float JET_b[]={0.5,0.5156862745098039,0.5313725490196078,0.5470588235294118,0.5627450980392157,0.5784313725490196,0.5941176470588235,0.6098039215686275,0.6254901960784314,0.6411764705882352,0.6568627450980392,0.6725490196078432,0.6882352941176471,0.7039215686274509,0.7196078431372549,0.7352941176470589,0.7509803921568627,0.7666666666666666,0.7823529411764706,0.7980392156862746,0.8137254901960784,0.8294117647058823,0.8450980392156863,0.8607843137254902,0.8764705882352941,0.892156862745098,0.907843137254902,0.9235294117647059,0.9392156862745098,0.9549019607843137,0.9705882352941176,0.9862745098039216,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0.9941176470588236,0.9784313725490197,0.9627450980392158,0.9470588235294117,0.9313725490196079,0.915686274509804,0.8999999999999999,0.884313725490196,0.8686274509803922,0.8529411764705883,0.8372549019607844,0.8215686274509804,0.8058823529411765,0.7901960784313726,0.7745098039215685,0.7588235294117647,0.7431372549019608,0.7274509803921569,0.7117647058823531,0.696078431372549,0.6803921568627451,0.6647058823529413,0.6490196078431372,0.6333333333333333,0.6176470588235294,0.6019607843137256,0.5862745098039217,0.5705882352941176,0.5549019607843138,0.5392156862745099,0.5235294117647058,0.5078431372549019,0.4921568627450981,0.4764705882352942,0.4607843137254903,0.4450980392156865,0.4294117647058826,0.4137254901960783,0.3980392156862744,0.3823529411764706,0.3666666666666667,0.3509803921568628,0.335294117647059,0.3196078431372551,0.3039215686274508,0.2882352941176469,0.2725490196078431,0.2568627450980392,0.2411764705882353,0.2254901960784315,0.2098039215686276,0.1941176470588237,0.1784313725490199,0.1627450980392156,0.1470588235294117,0.1313725490196078,0.115686274509804,0.1000000000000001,0.08431372549019622,0.06862745098039236,0.05294117647058805,0.03725490196078418,0.02156862745098032,0.00588235294117645,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

  for(uint32_t i=0; i<256; ++i)
  {
    JET_r_[i] = JET_r[i];
    JET_g_[i] = JET_g[i];
    JET_b_[i] = JET_b[i];
  }

}

