#ifndef SHADOWS_H
#define SHADOWS_H

#include "movingobject.h"
#include "json11.hpp"

using namespace cv;

struct ShadowsParameters
{
    float gradientThreshold, gradientThresholdMultiplier, lambda, tau, alpha, luminanceThreshold;
    bool edgeCorrection, autoGradientThreshold, randomReconstruction;
    int minObjectSize, minSegmentSize;

    void parse(const json11::Json& json);
};

class Shadows
{
    private:
        ShadowsParameters params;
                
        void findSegment(MovingObject& object, Point startPoint, InputOutputArray _segmentLabels, 
                uint16_t label, float gradientThreshold);
        void fillInBlanks(InputArray _fgMask, InputArray _mask);
        void minimizeObjectMask(MovingObject& obj);
        void showSegmentation(int nSegments, InputArray _labels);

        Mat D;
        
    public:
        Shadows(const json11::Json& jsonString);
        void updateParameters(const json11::Json& jsonString);
        void removeShadows(InputArray _src, InputArray _bg, InputArray _bgStdDev, InputArray _fgMask, 
                InputArray _objectLabels, std::vector<MovingObject>& movingObjects, OutputArray _dst);
};

#endif

/* vim: set ft=cpp ts=4 sw=4 sts=4 tw=0 fenc=utf-8 et: */
