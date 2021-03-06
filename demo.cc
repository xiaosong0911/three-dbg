#include "threedbg.h"

#include <thread>

int main() {
    threedbg::showGui = true;
    threedbg::init();
    for (float time = 0; threedbg::working() && time < 10; time += 0.025) {
        printf("time: %f\n", time);
        {
            {
                std::unique_ptr<PointsDrawerFactory> pdf = std::make_unique<PointsDrawerFactory>();
                pdf->particleRadius = 0.3;
                size_t particleNumber = 1;
                for (int x = -5; x <= 5; x++)
                    for (int y = -5; y <= 5; y++)
                        for (int z = -5; z <= 5; z++)
                            pdf->addPoint(glm::fvec3(x,y,z)/5.f + (float)sin(time), glm::fvec3(0.6));
                threedbg::addDrawerFactory("points", std::move(pdf));
            }
            {
                std::unique_ptr<LinesDrawerFactory> ldf = std::make_unique<LinesDrawerFactory>();
                for (int x = -2; x <= 2; x++)
                    for (int y = -2; y <= 2; y++)
                        for (int z = -2; z <= 2; z++)
                            ldf->addAxes({x,y,z}, 0.6);
                threedbg::addDrawerFactory("lines", std::move(ldf));
            }
            int w, h; std::vector<unsigned char> pixels;
            threedbg::snapshot(w, h, pixels);
        }
    }
    threedbg::free(false);
}
