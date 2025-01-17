#pragma once

#include <TwoStageFFTConvolver.h>

#include <GraphNode.h>
#include <Invariant.h>
#include <SingleWriterSingleReaderQueue.h>


namespace elem
{

    template <typename FloatType>
    struct ConvolutionNode : public GraphNode<FloatType> {
        using GraphNode<FloatType>::GraphNode;

        void setProperty(std::string const& key, js::Value const& val, SharedResourceMap<FloatType>& resources) override
        {
            GraphNode<FloatType>::setProperty(key, val);

            if (key == "path") {
                invariant(val.isString(), "path prop must be a string");
                invariant(resources.has((js::String) val), "failed to find a resource at the given path");

                auto ref = resources.get((js::String) val);
                auto co = std::make_shared<fftconvolver::TwoStageFFTConvolver>();

                co->reset();
                co->init(512, 4096, ref->data(), ref->size());

                convolverQueue.push(std::move(co));
            }
        }

        void process (BlockContext<FloatType> const& ctx) override {
            auto** inputData = ctx.inputData;
            auto* outputData = ctx.outputData;
            auto numChannels = ctx.numInputChannels;
            auto numSamples = ctx.numSamples;

            // First order of business: grab the most recent convolver to use if
            // there's anything in the queue. This behavior means that changing the convolver
            // impulse response while playing will cause a discontinuity.
            while (convolverQueue.size() > 0)
                convolverQueue.pop(convolver);

            if (numChannels == 0 || convolver == nullptr)
                return (void) std::fill_n(outputData, numSamples, FloatType(0));

            convolver->process(inputData[0], outputData, numSamples);
        }

        SingleWriterSingleReaderQueue<std::shared_ptr<fftconvolver::TwoStageFFTConvolver>> convolverQueue;
        std::shared_ptr<fftconvolver::TwoStageFFTConvolver> convolver;
    };

} // namespace elem
