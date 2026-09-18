#ifndef RKNNPOOL_H
#define RKNNPOOL_H

#include "ThreadPool.hpp"
#include <vector>
#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
// rknnModel模型类, inputType模型输入类型, outputType模型输出类型
template <typename rknnModel, typename inputType, typename outputType>
class rknnPool
{
private:
    int threadNum;
    std::string modelPath;
    std::vector<int> coreIds;

    long long id;
    std::mutex idMtx;
    std::unique_ptr<dpool::ThreadPool> pool;//初始化线程池哦
    std::atomic<int> pending_count{0};
    std::vector<std::shared_ptr<rknnModel>> models;

protected:
    int getModelId();

public:
    rknnPool(const std::string modelPath, int threadNum);
    rknnPool(const std::string modelPath, int threadNum, std::vector<int> coreIds);
    int init();
    // 模型推理/Model inference
    int put(inputType inputData, std::function<void(outputType)> onComplete);
    int get_task_size() 
    {
        return pending_count.load();
    }
    
    ~rknnPool();
};

template <typename rknnModel, typename inputType, typename outputType>
rknnPool<rknnModel, inputType, outputType>::rknnPool(const std::string modelPath, int threadNum) //构造函数
{
    this->modelPath = modelPath;
    this->threadNum = threadNum;
    this->id = 0;
}

template <typename rknnModel, typename inputType, typename outputType>
rknnPool<rknnModel, inputType, outputType>::rknnPool(const std::string modelPath, int threadNum, std::vector<int> coreIds) //构造函数
{
    this->modelPath = modelPath;
    this->threadNum = threadNum;
    this->coreIds = std::move(coreIds);
    this->id = 0;
}

template <typename rknnModel, typename inputType, typename outputType>
int rknnPool<rknnModel, inputType, outputType>::init()
{
    try
    {
        this->pool = std::make_unique<dpool::ThreadPool>(this->threadNum); //在这里初始化了线程池
        for (int i = 0; i < this->threadNum; i++)
            models.push_back(std::make_shared<rknnModel>(this->modelPath.c_str()));//一个model就是一个rknn对象
    }
    catch (const std::bad_alloc &e)
    {
        std::cout << "Out of memory: " << e.what() << std::endl;
        return -1;
    }
    // 初始化模型/Initialize the model
    for (int i = 0, ret = 0; i < threadNum; i++)
    {
        int coreId = coreIds.empty() ? -1 : coreIds[i % coreIds.size()];
        ret = models[i]->init(models[0]->get_pctx(), i != 0, coreId);
        if (ret != 0)
            return ret;
    }

    return 0;
}

template <typename rknnModel, typename inputType, typename outputType>
int rknnPool<rknnModel, inputType, outputType>::getModelId()
{
    std::lock_guard<std::mutex> lock(idMtx);
    int modelId = id % threadNum;
    id++;
    return modelId;
}

template <typename rknnModel, typename inputType, typename outputType>
int rknnPool<rknnModel, inputType, outputType>::put(inputType inputData, std::function<void(outputType)> onComplete)
{
    auto model = models[this->getModelId()];
    pending_count++;

    pool->submit([this, model, inputData, onComplete = std::move(onComplete)]() mutable 
    {
        outputType output = model->infer(inputData);
        onComplete(std::move(output));
        pending_count--;
    });

    return 0;
}

template <typename rknnModel, typename inputType, typename outputType>
rknnPool<rknnModel, inputType, outputType>::~rknnPool()
{
    while (pending_count.load() > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

#endif
