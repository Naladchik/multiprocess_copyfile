#pragma once
#include <utility>

// CRTP interface definition
template <typename Derived>
class ProcessorBase {
public:
    // 3 non-virtual member methods
    template <typename Input, typename Output>
    bool ProcessData(const Input& input, Output& output) {
        return static_cast<Derived*>(this)->ProcessDataImpl(input, output);
    }

    void NotifyComplete() {
        static_cast<Derived*>(this)->NotifyCompleteImpl();
    }

    template <typename Data>
    bool WaitNextData(Data& data) {
        return static_cast<Derived*>(this)->WaitNextDataImpl(data);
    }
};

// Unified Pipeline Stage that delegates to an Inner Policy
template <typename Inner>
class PipelineStage : public ProcessorBase<PipelineStage<Inner>> {
private:
    Inner inner_;

public:
    template <typename... Args>
    explicit PipelineStage(Args&&... args) : inner_(std::forward<Args>(args)...) {}

    template <typename Input, typename Output>
    bool ProcessDataImpl(const Input& input, Output& output) {
        return inner_.Process(input, output);
    }

    void NotifyCompleteImpl() {
        inner_.Complete();
    }

    template <typename Data>
    bool WaitNextDataImpl(Data& data) {
        return inner_.Wait(data);
    }

    Inner& GetInner() { return inner_; }
};