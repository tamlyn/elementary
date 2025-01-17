#include <emscripten/bind.h>

#include <memory>
#include <Runtime.h>

#include "Convolve.h"
#include "FFT.h"
#include "Metro.h"
#include "SampleTime.h"


using namespace emscripten;

//==============================================================================
/** The main processor for the WASM DSP. */
class ElementaryAudioProcessor
{
public:
    //==============================================================================
    ElementaryAudioProcessor(int numIns, int numOuts)
    {
        numInputChannels = static_cast<size_t>(numIns);
        numOutputChannels = static_cast<size_t>(numOuts);
    }

    ~ElementaryAudioProcessor() = default;

    //==============================================================================
    /** Called before processing starts. */
    void prepare (double sampleRate, unsigned int maxBlockSize)
    {
        scratchBuffers.clear();
        scratchPointers.clear();

        for (int i = 0; i < (numInputChannels + numOutputChannels); ++i)
            scratchBuffers.push_back(std::vector<float>(maxBlockSize));

        for (int i = 0; i < (numInputChannels + numOutputChannels); ++i)
            scratchPointers.push_back(scratchBuffers[i].data());

        // Configure the runtime
        runtime = std::make_unique<elem::Runtime<float>>(sampleRate, maxBlockSize);

        // Register extension nodes
        runtime->registerNodeType("convolve", [](elem::NodeId const id, double fs, int const bs) {
            return std::make_shared<elem::ConvolutionNode<float>>(id, fs, bs);
        });

        runtime->registerNodeType("fft", [](elem::NodeId const id, double fs, int const bs) {
            return std::make_shared<elem::FFTNode<float>>(id, fs, bs);
        });

        runtime->registerNodeType("metro", [](elem::NodeId const id, double fs, int const bs) {
            return std::make_shared<elem::MetronomeNode<float>>(id, fs, bs);
        });

        runtime->registerNodeType("time", [](elem::NodeId const id, double fs, int const bs) {
            return std::make_shared<elem::SampleTimeNode<float>>(id, fs, bs);
        });
    }

    //==============================================================================
    /** Returns a Float32Array view into the internal work buffer data. */
    val getInputBufferData (int index)
    {
        auto len = scratchBuffers[index].size();
        auto* data = scratchBuffers[index].data();

        return val(typed_memory_view(len, data));
    }

    /** Returns a Float32Array view into the internal work buffer data. */
    val getOutputBufferData (int index)
    {
        auto len = scratchBuffers[numInputChannels + index].size();
        auto* data = scratchBuffers[numInputChannels + index].data();

        return val(typed_memory_view(len, data));
    }

    //==============================================================================
    /** Message batch handling. */
    void postMessageBatch (val payload, val errorCallback)
    {
        auto v = emValToValue(payload);

        if (!v.isArray()) {
            errorCallback(val("error"), val("Malformed message batch."));
            return;
        }

        auto const& batch = v.getArray();

        try {
            runtime->applyInstructions(batch);
        } catch (elem::InvariantViolation const& e) {
            errorCallback(val("error"), val(e.what()));
        } catch (mpark::bad_variant_access const& e) {
            errorCallback(val("error"), val("Bad variant access"));
        } catch (...) {
            errorCallback(val("error"), val("Unhandled exception"));
        }
    }

    void reset()
    {
        runtime->reset();
    }

    void updateSharedResourceMap(val path, val buffer, val errorCallback)
    {
        auto p = emValToValue(path);
        auto b = emValToValue(buffer);

        if (!p.isString())
            return (void) errorCallback(val("Path must be a string type"));

        if (!b.isArray() && !b.isFloat32Array())
            return (void) errorCallback(val("Buffer argument must be an Array or Float32Array type"));

        try {
            auto buf = b.isArray() ? arrayToFloatVector(b.getArray()) : b.getFloat32Array();
            runtime->updateSharedResourceMap((elem::js::String) p, buf.data(), buf.size());
        } catch (elem::InvariantViolation const& e) {
            errorCallback(val("Invalid buffer for updating resource map"));
        }
    }

    /** Audio block processing. */
    void process (int const numSamples)
    {
        // We just operate on our scratch data. Expect the JavaScript caller to hit
        // our getInputBufferData and getOutputBufferData to prepare and extract the actual
        // data for this processor
        runtime->process(
            const_cast<const float**>(scratchPointers.data()),
            numInputChannels,
            scratchPointers.data() + numInputChannels,
            numOutputChannels,
            numSamples,
            static_cast<void*>(&sampleTime)
        );

        sampleTime += static_cast<int64_t>(numSamples);
    }

    /** Callback events. */
    void processQueuedEvents(val callback)
    {
        elem::js::Array batch;

        runtime->processQueuedEvents([this, &batch](std::string const& type, elem::js::Value evt) {
            batch.push_back(elem::js::Array({type, evt}));
        });

        callback(valueToEmVal(batch));
    }

private:
    //==============================================================================
    std::vector<float> arrayToFloatVector (elem::js::Array const& ar)
    {
        try {
            std::vector<float> ret (ar.size());

            for (size_t i = 0; i < ar.size(); ++i) {
                ret[i] = static_cast<float>((elem::js::Number) ar[i]);
            }

            return ret;
        } catch (std::exception const& e) {
            throw elem::InvariantViolation("Failed to convert Array to float vector; invalid array child!");
        }
    }

    elem::js::Value emValToValue (val const& v)
    {
        if (v.isUndefined())
            return elem::js::Undefined();
        if (v.isNull())
            return elem::js::Null();
        if (v.isTrue())
            return elem::js::Value(true);
        if (v.isFalse())
            return elem::js::Value(false);
        if (v.isNumber())
            return elem::js::Value(v.as<double>());
        if (v.isString())
            return elem::js::Value(v.as<std::string>());
        if (v.instanceof(val::global("Float32Array"))) {
            // This conversion function is part of the emscripten namespace for
            // mapping from emscripten::val to a simple std::vector.
            return elem::js::Value(convertJSArrayToNumberVector<float>(v));
        }

        if (v.isArray())
        {
            auto const length = v["length"].as<int>();
            elem::js::Array ret;

            for (int i = 0; i < length; ++i)
            {
                ret.push_back(emValToValue(v[i]));
            }

            return ret;
        }

        // We don't support functions yet...
        if (v.instanceof(val::global("Function"))) {
            return elem::js::Undefined();
        }

        // This case must come at the end, because Arrays, Functions, Float32Arrays,
        // etc are all Objects too
        if (v.instanceof(val::global("Object"))) {
            auto const keys = val::global("Object").call<val>("keys", v);
            auto const numKeys = keys["length"].as<size_t>();

            elem::js::Object ret;

            for (size_t i = 0; i < numKeys; ++i) {
                ret.insert({keys[i].as<std::string>(), emValToValue(v[keys[i]])});
            }

            return ret;
        }

        return elem::js::Undefined();
    }

    val valueToEmVal (elem::js::Value const& v)
    {
        if (v.isUndefined())
            return val::undefined();
        if (v.isNull())
            return val::null();
        if (v.isBool())
            return val((bool) v);
        if (v.isNumber())
            return val((elem::js::Number) v);
        if (v.isString())
            return val((elem::js::String) v);

        if (v.isArray())
        {
            auto& va = v.getArray();
            auto ret = val::array();

            for (size_t i = 0; i < va.size(); ++i)
            {
                ret.set(i, valueToEmVal(va[i]));
            }

            return ret;
        }

        if (v.isFloat32Array())
        {
            auto& va = v.getFloat32Array();

            auto opts = val::object();
            opts.set("length", va.size());

            auto ret = val::global("Float32Array").call<val>("from", opts);

            for (size_t i = 0; i < va.size(); ++i)
            {
                ret.set(i, val(va[i]));
            }

            return ret;
        }

        if (v.isObject())
        {
            auto& vo = v.getObject();
            auto ret = val::object();

            for (auto const& [key, x] : vo)
            {
                ret.set(key, valueToEmVal(x));
            }

            return ret;
        }

        // Function types not supported!
        return val::undefined();
    }

    //==============================================================================
    std::unique_ptr<elem::Runtime<float>> runtime;
    std::vector<std::vector<float>> scratchBuffers;
    std::vector<float*> scratchPointers;

    int64_t sampleTime = 0;

    size_t numInputChannels = 0;
    size_t numOutputChannels = 2;
};

EMSCRIPTEN_BINDINGS(Elementary) {
    class_<ElementaryAudioProcessor>("ElementaryAudioProcessor")
        .constructor<int, int>()
        .function("prepare", &ElementaryAudioProcessor::prepare)
        .function("getInputBufferData", &ElementaryAudioProcessor::getInputBufferData)
        .function("getOutputBufferData", &ElementaryAudioProcessor::getOutputBufferData)
        .function("postMessageBatch", &ElementaryAudioProcessor::postMessageBatch)
        .function("reset", &ElementaryAudioProcessor::reset)
        .function("updateSharedResourceMap", &ElementaryAudioProcessor::updateSharedResourceMap)
        .function("process", &ElementaryAudioProcessor::process)
        .function("processQueuedEvents", &ElementaryAudioProcessor::processQueuedEvents);
};
