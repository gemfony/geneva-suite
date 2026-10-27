/**
 * @file PlotRNGDistributions.cpp
 */

/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

/**
 * This test creates nEntries random numbers each for different random
 * number distributions. The results is output in the ROOT format -- see
 * http://root.cern.ch for further information. When executing the ROOT
 * script, a number of PNGs are created, suitable for inclusion in the
 * documentation.
 */

// Standard header files
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

// Boost header files

// Geneva header files
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Hap;
using namespace boost;

enum class distType : Gem::Common::ENUMBASETYPE {
    GAUSSIAN,
    DOUBLEGAUSSIAN,
    EVEN,
    EVENWITHBOUNDARIES,
    DISCRETE,
    DISCRETEBOUND,
    BITPROB,
    BITSIMPLE
};

template <class T>
// NOLINTNEXTLINE(readability-function-size) -- one switch-driven dispatcher, one case per distribution type sampled into the same output vector
void createRandomVector(
    std::vector<T> &vec_t,
    const distType &dType,
    const std::size_t &nEntries,
    const std::shared_ptr<Gem::Hap::GRandomBase>& gr_ptr
) {
    std::size_t i = 0;

    std::normal_distribution<double> normal_distribution(0., 0.5);
    Gem::Hap::bi_normal_distribution<double> bi_normal_distribution(0., 0.5, 0.5, 2.);
    std::uniform_real_distribution<double> uniform_real_distribution_01;
    std::uniform_int_distribution<std::int32_t> uniform_int_distribution;

    switch(dType) {
    case distType::GAUSSIAN: // standard distribution
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(T(normal_distribution(*gr_ptr)));
        }
        break;

    case distType::DOUBLEGAUSSIAN:
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(T(bi_normal_distribution(*gr_ptr)));
        } // (mean, sigma, distance)
        break;

    case distType::EVEN: // double in the range [0,1[
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(T(uniform_real_distribution_01(*gr_ptr)));
        }
        break;

    case distType::EVENWITHBOUNDARIES: // double in the range [-3,2[
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(T(uniform_real_distribution_01(
                *gr_ptr,
                std::uniform_real_distribution<double>::param_type(-3., 2.)
            )));
        }
        break;

    case distType::DISCRETE:
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(uniform_int_distribution(
                *gr_ptr,
                std::uniform_int_distribution<std::int32_t>::param_type(0, 10)
            ));
        }
        break;

    case distType::DISCRETEBOUND:
        for(i = 0; i < nEntries; i++) {
            vec_t.push_back(uniform_int_distribution(
                *gr_ptr,
                std::uniform_int_distribution<std::int32_t>::param_type(-3, 10)
            ));
        }
        break;

    case distType::BITPROB: {
        // Note: A probability of 0.7 results in approximately 70% "true" values
        std::bernoulli_distribution weighted_bool(0.7);
        for(i = 0; i < nEntries; i++) {
            if(weighted_bool(*gr_ptr)) {
                vec_t.push_back(1);
            }
            else {
                vec_t.push_back(0);
            }
        }
    } break;

    case distType::BITSIMPLE: {
        std::bernoulli_distribution uniform_bool; // defaults to 0.5
        for(i = 0; i < nEntries; i++) {
            if(uniform_bool(*gr_ptr)) {
                vec_t.push_back(1);
            }
            else {
                vec_t.push_back(0);
            }
        }
    } break;
    };
}

// NOLINTNEXTLINE(readability-function-size) -- main() of a doc-program: writes the fixed ROOT-macro header, samples every distribution kind, then emits the per-distribution Fill()/labelling blocks
int main(int argc, char **argv) {
    std::shared_ptr<Gem::Hap::GRandomBase> gr_ptr;

    const std::size_t nEntries = 6000;
    const std::uint16_t nProducerThreads = 4;

    std::size_t i = 0;
    std::vector<double> gaussian;
    std::vector<double> doublegaussian;
    std::vector<double> even;
    std::vector<double> evenwithboundaries;
    std::vector<std::int32_t> discrete;
    std::vector<std::int32_t> discretebound;
    std::vector<std::int32_t> bitprob;
    std::vector<std::int32_t> bitsimple;

    randomFactory()->setNProducerThreads(nProducerThreads);

    // Create a random number proxy
    gr_ptr = std::shared_ptr<GRandomT<randomSource::QUEUE>>(
        new GRandomT<randomSource::QUEUE>()
    );

    std::ofstream ofs("rootPlotRNGDistributions.C");
    if(!ofs) {
        glogger << "Error: Could not write file" << '\n' << GWARNING;
        return 1;
    }

    // The header of the root file
    ofs << "{" << '\n'
        << "  gROOT->Reset();" << '\n'
        << "  gStyle->SetOptTitle(0);" << '\n'
        << "  gStyle->SetOptStat(0);" << '\n'
        << "  gStyle->SetCanvasColor(0);" << '\n'
        << "  gStyle->SetStatBorderSize(1);" << '\n'
        << '\n'
        << "  TCanvas *cc = new TCanvas(\"cc\",\"cc\",0,0,1200,800);" << '\n'
        << '\n'
        << "  TH1F *gauss = new TH1F(\"gauss\",\"gauss\",200,-2.6,2.6);" << '\n'
        << "  TH1F *dgauss = new TH1F(\"dgauss\",\"dgauss\",200,-4.,4.);" << '\n'
        << "  TH1F *even = new TH1F(\"even\",\"even\",200,-0.5,1.5);" << '\n'
        << "  TH1F *evenwb = new TH1F(\"evenwb\",\"evenwb\",200,-3.5,2.5);" << '\n'
        << "  TH1I *discrete = new TH1I(\"discrete\",\"discrete\",12,-1,10);" << '\n'
        << "  TH1I *discretewb = new TH1I(\"discretewb\",\"discretewb\",16,-4,11);" << '\n'
        << "  TH1I *bitprob = new TH1I(\"bitprob\",\"bitprob\",4,-1,2);" << '\n'
        << "  TH1I *bitsimple = new TH1I(\"bitsimple\",\"bitsimple\",4,-1,2);" << '\n'
        << '\n';

    createRandomVector<double>(gaussian, distType::GAUSSIAN, nEntries, gr_ptr);
    createRandomVector<double>(doublegaussian, distType::DOUBLEGAUSSIAN, nEntries, gr_ptr);
    createRandomVector<double>(even, distType::EVEN, nEntries, gr_ptr);
    createRandomVector<double>(evenwithboundaries, distType::EVENWITHBOUNDARIES, nEntries, gr_ptr);
    createRandomVector<std::int32_t>(discrete, distType::DISCRETE, nEntries, gr_ptr);
    createRandomVector<std::int32_t>(discretebound, distType::DISCRETEBOUND, nEntries, gr_ptr);
    createRandomVector<std::int32_t>(bitprob, distType::BITPROB, nEntries, gr_ptr);
    createRandomVector<std::int32_t>(bitsimple, distType::BITSIMPLE, nEntries, gr_ptr);

    for(i = 0; i < nEntries; i++) {
        ofs << "  gauss->Fill(" << gaussian.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  dgauss->Fill(" << doublegaussian.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  even->Fill(" << even.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  evenwb->Fill(" << evenwithboundaries.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  discrete->Fill(" << discrete.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  discretewb->Fill(" << discretebound.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  bitprob->Fill(" << bitprob.at(i) << ");" << '\n';
    }
    ofs << '\n';

    for(i = 0; i < nEntries; i++) {
        ofs << "  bitsimple->Fill(" << bitsimple.at(i) << ");" << '\n';
    }
    ofs << '\n';

    //---------------------------------------------------------------------------------

    ofs << "  gauss->GetXaxis()->SetTitle(\"x\");" << '\n'
        << "  gauss->GetYaxis()->SetTitle(\"number of entries\");" << '\n'
        << "  gauss->GetYaxis()->SetTitleOffset(1.2);" << '\n'
        << "  gauss->Draw();" << '\n'
        << "  TLatex *gaussText1 = new TLatex(0.8,1200,\"Normal distribution\");" << '\n'
        << "  TLatex *gaussText2 = new TLatex(0.8,1115,\"with mean=0, #sigma=0.5\");" << '\n'
        << "  gaussText1->SetTextSize(0.035);" << '\n'
        << "  gaussText2->SetTextSize(0.035);" << '\n'
        << "  gaussText1->Draw();" << '\n'
        << "  gaussText2->Draw();" << '\n'
        << "  gPad->Update();" << '\n'
        << "  double ymax = gPad->GetUymax();" << '\n'
        << "  TLine *gaussLine = new TLine(0.,0.,0., ymax);" << '\n'
        << "  gaussLine->SetLineStyle(2);" << '\n'
        << "  gaussLine->Draw();" << '\n'
        << "  cc->SaveAs(\"gauss.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  dgauss->GetXaxis()->SetTitle(\"x\");" << '\n'
        << "  dgauss->GetYaxis()->SetTitle(\"number of entries\");" << '\n'
        << "  dgauss->GetYaxis()->SetTitleOffset(1.2);" << '\n'
        << "  dgauss->Draw();" << '\n'
        << "  TLatex *dgaussText1 = new TLatex(1.5, 970, \"Two normal distributions with\");"
        << '\n'
        << "  TLatex *dgaussText2 = new TLatex(1.5, 920, \"mean=0.5, #sigma=0.5 and distance\");"
        << '\n'
        << "  TLatex *dgaussText3 = new TLatex(1.5, 870, \"between the mean values of 2\");"
        << '\n'
        << "  dgaussText1->SetTextSize(0.025);" << '\n'
        << "  dgaussText2->SetTextSize(0.025);" << '\n'
        << "  dgaussText3->SetTextSize(0.025);" << '\n'
        << "  dgaussText1->Draw();" << '\n'
        << "  dgaussText2->Draw();" << '\n'
        << "  dgaussText3->Draw();" << '\n'
        << "  gPad->Update();" << '\n'
        << "  TLine *dgaussLine1 = new TLine(-1.,0.,-1., ymax);" << '\n'
        << "  dgaussLine1->SetLineStyle(2);" << '\n'
        << "  dgaussLine1->Draw();" << '\n'
        << "  TLine *dgaussLine2 = new TLine(1.,0.,1., ymax);" << '\n'
        << "  dgaussLine2->SetLineStyle(2);" << '\n'
        << "  dgaussLine2->Draw();" << '\n'
        << "  cc->SaveAs(\"dgauss.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  even->Draw();" << '\n'
        << "  cc->SaveAs(\"even.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  evenwb->Draw();" << '\n'
        << "  cc->SaveAs(\"evenwb.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  discrete->Draw();" << '\n'
        << "  cc->SaveAs(\"discrete.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  discretewb->Draw();" << '\n'
        << "  cc->SaveAs(\"discretewb.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  bitprob->Draw();" << '\n'
        << "  cc->SaveAs(\"bitprob.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "  bitsimple->Draw();" << '\n'
        << "  cc->SaveAs(\"bitsimple.png\");"
        << '\n'

        //---------------------------------------------------------------------------------

        << "}" << '\n';

    ofs.close();

    return 0;
}
