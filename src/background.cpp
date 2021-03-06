#include <opencv2/imgproc.hpp>
#include <cstdlib>
#include "background.h"

void BackgroundParameters::parse(const json11::Json& json)
{
    initialVariance = json["initialVariance"].number_value();
    initialWeight = json["initialWeight"].number_value();
    learningRate = json["learningRate"].number_value();
    foregroundThreshold = json["foregroundThreshold"].number_value();
    medianFilterSize = json["medianFilterSize"].int_value();
    morphFilterSize = json["morphFilterSize"].int_value();

    if (morphFilterSize != 0)
        morphFilterKernel = getStructuringElement(MORPH_ELLIPSE, Size(morphFilterSize, morphFilterSize));
}

Background::Background(const Size& size, const json11::Json& json) :
    etaConst(pow(2 * M_PI, 3.0 / 2.0))
#ifdef MULTITHREADING
    ,threadPool(5)
#endif
{
    params.parse(json);

#ifdef SIMD
    posix_memalign((void**)&gaussians, 16, size.area() * 5 * sizeof(float) * GAUSSIANS_PER_PIXEL);
#else
    gaussians = new GaussianMixture[size.area()];
#endif

    currentBackground = Mat::zeros(size, CV_8UC3);
    currentStdDev = Mat::zeros(size, CV_32F);

#ifdef MULTITHREADING    
    nThreads = std::thread::hardware_concurrency();
#endif
}

Background::~Background()
{
#ifdef SIMD
    free(gaussians);
#else
    delete[] gaussians;
#endif
}

void Background::updateParameters(const json11::Json& json)
{
    params.parse(json);
}

void Background::processFrame(InputArray _src, OutputArray _foregroundMask)
{
    Mat src = _src.getMat(), foregroundMask = _foregroundMask.getMat();

    for (int row = 0; row < src.rows; ++row)
    {
        uint8_t *foregroundMaskPtr = foregroundMask.ptr<uint8_t>(row);
        uint8_t *currentBackgroundPtr = currentBackground.ptr<uint8_t>(row);
        float *currentStdDevPtr = currentStdDev.ptr<float>(row);
        uint8_t *srcPtr = src.ptr<uint8_t>(row);
        int idx = src.cols * row; 

        for (int col = 0; col < src.cols; col++, idx++)
        {
            Vec3b bgr;
            bgr[0] = *srcPtr++; 
            bgr[1] = *srcPtr++; 
            bgr[2] = *srcPtr++; 
                
            GaussianMixture& mixture = gaussians[idx];
            bool foreground = processPixel(bgr, mixture);
            *foregroundMaskPtr++ = foreground ? 1 : 0;

            // update current background model (or rather, background image)
            const Gaussian& gauss = *std::max_element(std::begin(mixture), std::end(mixture), 
                    [](const Gaussian& a, const Gaussian& b) { return a.weight < b.weight; });

            *currentBackgroundPtr++ = gauss.meanB;
            *currentBackgroundPtr++ = gauss.meanG;
            *currentBackgroundPtr++ = gauss.meanR;
            *currentStdDevPtr++ = sqrt(gauss.variance);
        }
    }

    if (params.medianFilterSize != 0)
        medianBlur(foregroundMask, foregroundMask, params.medianFilterSize);
    if (params.morphFilterSize != 0)
        erode(foregroundMask, foregroundMask, params.morphFilterKernel);
}

void Background::processFrameSIMD(InputArray _src, OutputArray _foregroundMask)
{
    Mat src = _src.getMat(), foregroundMask = _foregroundMask.getMat();
    uint32_t nPixels = src.size().area();

#ifdef MULTITHREADING
    uint32_t pixelsPerThread = nPixels / nThreads;
    std::vector<std::future<void>> results;

    for (int i = 0; i < nThreads; i++)
    {
        uint32_t startIdx = pixelsPerThread * i;
        uint32_t endIdx = startIdx + pixelsPerThread; 

        results.emplace_back(threadPool.enqueue([=]()
        {
            for (uint32_t idx = startIdx; idx < endIdx; idx += 4)
            {
                uint32_t fgMask = processPixels_SSE2(src.data + 3*idx,
                                            (float*)gaussians + 5*GAUSSIANS_PER_PIXEL*idx,
                                            currentBackground.data + 3*idx,
                                            (float*)currentStdDev.data + idx,
                                            params.learningRate, params.initialVariance,
                                            params.initialWeight, params.foregroundThreshold);

                *((uint32_t*)foregroundMask.data + idx/4) = fgMask;
            }
        }));
    }

    for(auto&& r: results)
        r.get();
#else
    for (uint32_t idx = 0; idx < nPixels; idx += 4)
    {
        uint32_t fgMask = processPixels_SSE2(src.data + 3*idx,
                                            (float*)gaussians + 5*GAUSSIANS_PER_PIXEL*idx,
                                            currentBackground.data + 3*idx,
                                            (float*)currentStdDev.data + idx,
                                            params.learningRate, params.initialVariance,
                                            params.initialWeight, params.foregroundThreshold);

        *((uint32_t*)foregroundMask.data + idx/4) = fgMask;
    }
#endif

    if (params.medianFilterSize != 0)
        medianBlur(foregroundMask, foregroundMask, params.medianFilterSize);
    if (params.morphFilterSize != 0)
        erode(foregroundMask, foregroundMask, params.morphFilterKernel);
}

bool Background::processPixel(const Vec3b& bgr, GaussianMixture& mixture)
{
    double weightSum = 0.0;
    bool matchFound = false;

    for (Gaussian& gauss : mixture)
    {
        float dB = gauss.meanB - bgr[0];
        float dG = gauss.meanG - bgr[1];
        float dR = gauss.meanR - bgr[2];
        float distance = dR*dR + dG*dG + dB*dB;

        if (sqrt(distance) < 2.5*sqrt(gauss.variance) && !matchFound)
        {
            matchFound = true;
            
            // determinant of covariance matrix (eq. 4 in Stauffer&Grimson's paper) equals to sigma^6. 
            // we need sigma^3, let's so calculate square root of variance (stdDev) and multiply it 3 times.
            float stdDev = sqrt(gauss.variance);

            float exponent = (-0.5 * distance) / gauss.variance;
            float eta = exp(exponent) / (etaConst * stdDev * stdDev * stdDev);

            float rho = params.learningRate * eta;
            float oneMinusRho = 1.0 - rho;

            gauss.meanB = oneMinusRho*gauss.meanB + rho*bgr[0];
            gauss.meanG = oneMinusRho*gauss.meanG + rho*bgr[1];
            gauss.meanR = oneMinusRho*gauss.meanR + rho*bgr[2];
            gauss.variance = oneMinusRho*gauss.variance + rho*distance;
        }
        else
        {
            gauss.weight = (1.0 - params.learningRate)*gauss.weight;
        }

        weightSum += gauss.weight;
    }

    if(!matchFound)
    {
        // pixel didn't match any of currently existing Gaussians.
        // as Grimson & Stauffer suggest, let's modify least probable distribution.
        
        Gaussian& gauss = *std::min_element(std::begin(mixture), std::end(mixture), 
            [](const Gaussian& a, const Gaussian& b) { return a.weight < b.weight; });

        gauss.meanB = bgr[0];
        gauss.meanG = bgr[1];
        gauss.meanR = bgr[2];   
        gauss.weight = params.initialWeight;
        gauss.variance = params.initialVariance;

        // Gaussian has been changed, update sum of the weights.
        weightSum = 0;
        for (const Gaussian& gauss : mixture)
            weightSum += gauss.weight;
    }

    // normalize the weights
    // sum of all weight is supposed to equal 1.
    float invWeightSum = 1.0 / weightSum;
    for (Gaussian& gauss : mixture)
        gauss.weight *= invWeightSum;

    // estimate whether pixel belongs to foreground using probability equation given by Benedek & Sziranyi
    // calculate eplison for background
    const Gaussian& gauss = *std::max_element(std::begin(mixture), std::end(mixture), 
            [](const Gaussian& a, const Gaussian& b) { return a.weight < b.weight; });

    float epsilon_bg = 2 * log(2 * M_PI);
    epsilon_bg += 3 * log(sqrt(gauss.variance));
    epsilon_bg += 0.5 * (bgr[0] - gauss.meanB) * (bgr[0] - gauss.meanB) / gauss.variance;
    epsilon_bg += 0.5 * (bgr[1] - gauss.meanG) * (bgr[1] - gauss.meanG) / gauss.variance;
    epsilon_bg += 0.5 * (bgr[2] - gauss.meanR) * (bgr[2] - gauss.meanR) / gauss.variance;

    return epsilon_bg > params.foregroundThreshold;
}

const Mat& Background::getCurrentBackground() const
{
    return currentBackground;
}

const Mat& Background::getCurrentStdDev() const
{
    return currentStdDev;
}

/* vim: set ft=cpp ts=4 sw=4 sts=4 tw=0 fenc=utf-8 et: */
