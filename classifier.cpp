#include "classifier.h"

Classifier::Classifier(const std::vector<Point>& points, const std::string& directionStr)
{
	collisionLines[0] = Line(0, points[0],  points[1]);
	collisionLines[1] = Line(1, points[2],  points[3]);
	
	naturalDirection = Direction(directionStr);
	oppositeDirection = !naturalDirection;
}

void Classifier::trackObjects(InputArray _frame, InputArray _mask, std::vector<MovingObject>& movingObjects)
{
	Mat frame = _frame.getMat(), mask = _mask.getMat();
	Mat grayFrame;
	cvtColor(frame, grayFrame, COLOR_BGR2GRAY);

	// predict next position for already recognised objects
	for (auto& object: classifiedObjects)
	{
		if (object.prevFeatures.size() > 0)
			object.predictNextPosition(prevFrame, grayFrame);
	}

	classifiedObjects.erase(std::remove_if(classifiedObjects.begin(), classifiedObjects.end(),
				[](MovingObject& o) { return o.remove; }), classifiedObjects.end());

	// iterate over all detected moving objects and try match them
	// to already known objects saved in 'classifiedObjects'.
	std::vector<MovingObject> objsToAdd;
	for (auto& object: movingObjects)
	{
		auto objMask = object.mask;
		bool objectMatched = false;

		// check if predicted feature positions are still within object mask.
		// if not - it's probably a different object.
		// sometimes two or more moving objects match to the same mask,
		// in this case - merge them.
		std::vector<MovingObject*> objectsToMerge;

		for (auto& classifiedObj: classifiedObjects)
		{
			// calculate intersection of two rectangles.
			// if area of this intersection is nonzero, the two rectangles contain one another in some way
			// and we can speculate that the two objects are indeed the same.
			if ((object.selector & classifiedObj.selector).area() > 0)
			{
				objectMatched = true;
				classifiedObj.mask = objMask;
				classifiedObj.minimizeMask();
				classifiedObj.collisions.insert(object.collisions.begin(), object.collisions.end());
				objectsToMerge.push_back(&classifiedObj);
#ifdef DEBUG
				std::cout << "ID " << classifiedObj.ID << " matched" << std::endl;
#endif
			}
		}

		// merge objects if necessary
		if (objectsToMerge.size() > 1)
		{
#ifdef DEBUG
			std::cout << objectsToMerge.size() << " objects to merge" << std::endl;
#endif

			auto obj = MovingObject(frame.size());
			obj.ID = objectsToMerge.front()->ID;
			obj.alreadyCounted = std::any_of(objectsToMerge.begin(), objectsToMerge.end(),
					[](const MovingObject* obj) { return obj->alreadyCounted; });

			for (MovingObject* o: objectsToMerge)
			{
				o->remove = true;
				obj.mask += o->mask;
				obj.collisions.insert(o->collisions.begin(), o->collisions.end());
			}
			
			obj.minimizeMask();
			obj.updateTrackedFeatures(grayFrame, frameCounter);
			objsToAdd.push_back(obj);
		}

		// in case some object isn't matched with already known objects,
		// add a new one to the list
		if (!objectMatched)
		{
			object.updateTrackedFeatures(grayFrame, frameCounter);
			if (object.features.size() == 0)
				continue;

			object.ID = objCounter++;
			objsToAdd.push_back(object);
		}
	}
	
	for (auto& obj: objsToAdd)
		classifiedObjects.push_back(obj);

	for (auto& obj: classifiedObjects)
	{
		if (obj.features.size() < 4 || frameCounter - obj.featuresLastUpdated >= 10)
			obj.updateTrackedFeatures(grayFrame, frameCounter);

		std::swap(obj.prevFeatures, obj.features);
	}
	
	prevFrame = grayFrame.clone();
	frameCounter++;
}

void Classifier::checkCollisions()
{
	for (auto& line: collisionLines)
	{
		bool anyOfObjectsCrossesTheLine = false;
		for (auto& obj: classifiedObjects)
		{
			if (line.intersect(obj.selector))
			{
				obj.collisions[line.ID] = frameCounter;
				anyOfObjectsCrossesTheLine = true;
			}
		}

		line.isBeingCrossed = anyOfObjectsCrossesTheLine;
	}
}

void Classifier::updateCounters()
{
	for (auto& obj: classifiedObjects)
	{
		if (obj.collisions.size() == 2 && !obj.alreadyCounted)
		{
			uint32_t line0Time = obj.collisions[0];
			uint32_t line1Time = obj.collisions[1];
			if (line0Time < line1Time)
				naturalDirection++;
			else
				oppositeDirection++;

			obj.alreadyCounted = true;
		}
	}
}

void Classifier::drawBoundingBoxes(InputOutputArray _frame)
{
	Mat frame = _frame.getMat();
	
	for (auto& obj: classifiedObjects)
	{
		rectangle(frame, obj.selector, Scalar(255, 0, 0), 2, 1);

		std::string text = std::to_string(obj.ID);
		int fontFace = FONT_HERSHEY_SCRIPT_SIMPLEX;
		double fontScale = 0.5;
		int thickness = 2;

		// center the text
		putText(frame, text, obj.selector.tl(), fontFace, fontScale,
		        Scalar::all(255), thickness, 8);

		for (auto& pt: obj.features)
			circle(frame, pt, 3, Scalar(255, 0, 255));
	}

	//	RotatedRect rect = minAreaRect(contour);
	//	Point2f vtx[4];
	//	rect.points(vtx);
	//	for(int i = 0; i < 4; i++)
	//		line(frame, vtx[i], vtx[(i+1)%4], Scalar(255, 0, 255), 2, LINE_AA);
}

void Classifier::drawCollisionLines(InputOutputArray _frame)
{
	Mat frame = _frame.getMat();
	
	for (auto& line: collisionLines)
		line.draw(frame);
}

void Classifier::drawCounters(InputOutputArray _frame)
{
	Mat frame = _frame.getMat();
	
	int fontFace = FONT_HERSHEY_SIMPLEX;
	double fontScale = 1.0;
	int thickness = 1;

	int baseline = 0;
	std::string text = naturalDirection.prettyString();
	Size textSize = getTextSize(text, fontFace,
			fontScale, thickness, &baseline);

	Point offset = Point(10, frame.size().height - 10);
	putText(frame, text, offset, fontFace, fontScale, Scalar::all(255), 
			thickness, LINE_AA, false);

	offset.y -= textSize.height + 5;
	putText(frame, oppositeDirection.prettyString(), offset, fontFace, fontScale, Scalar::all(255), 
			thickness, LINE_AA, false);
}
