#pragma once

#include "DmlGraphFusionHelper.h"
#include "DmlRuntimeFusedGraphKernel.h"

namespace Dml
{
namespace DmlGraphFusionHelper
{
    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        ORT_THROW_IF_FAILED(provider->UploadToResource(buffer.Get(), tensorPtr, tensorByteSize));

        return buffer;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateCpuResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        // Map the buffer and copy the data
        void* bufferData = nullptr;
        D3D12_RANGE range = {0, tensorByteSize};
        ORT_THROW_IF_FAILED(buffer->Map(0, &range, &bufferData));
        memcpy(bufferData, tensorPtr, tensorByteSize);
        buffer->Unmap(0, &range);

        return buffer;
    }

    void UnwrapTensor(
        Windows::AI::MachineLearning::Adapter::IWinmlExecutionProvider* winmlProvider,
        const onnxruntime::Tensor* tensor,
        ID3D12Resource** resource,
        uint64_t* allocId)
    {
        IUnknown* allocationUnk = static_cast<IUnknown*>(const_cast<void*>(tensor->DataRaw()));
        Microsoft::WRL::ComPtr<IUnknown> resourceUnk;
        winmlProvider->GetABIDataInterface(false, allocationUnk, &resourceUnk);

        *allocId = winmlProvider->TryGetPooledAllocationId(allocationUnk, 0);

        ORT_THROW_IF_FAILED(resourceUnk->QueryInterface(resource));
    }

    std::tuple<std::unique_ptr<std::byte[]>, std::vector<uint8_t>, std::byte*, size_t> UnpackInitializer(
        const onnxruntime::Graph& graph,
        const ONNX_NAMESPACE::TensorProto* initializer)
    {
        std::unique_ptr<std::byte[]> unpackedTensor;
        std::vector<uint8_t> unpackedExternalTensor;
        std::byte* tensorPtr = nullptr;
        size_t tensorByteSize = 0;

        // The tensor may be stored as raw data or in typed fields.
        if (initializer->data_location() == onnx::TensorProto_DataLocation_EXTERNAL)
        {
            THROW_IF_NOT_OK(onnxruntime::utils::UnpackInitializerData(*initializer, graph.ModelPath(), unpackedExternalTensor));
            tensorPtr = reinterpret_cast<std::byte*>(unpackedExternalTensor.data());
            tensorByteSize = unpackedExternalTensor.size();
        }
        else if (initializer->has_raw_data())
        {
            tensorPtr = (std::byte*)(initializer->raw_data().c_str());
            tensorByteSize = initializer->raw_data().size();
        }
        else
        {
            std::tie(unpackedTensor, tensorByteSize) = Windows::AI::MachineLearning::Adapter::UnpackTensor(*initializer, graph.ModelPath());
            tensorPtr = unpackedTensor.get();
        }

        return std::make_tuple(std::move(unpackedTensor), std::move(unpackedExternalTensor), tensorPtr, tensorByteSize);
    }

    void ProcessInputData(
        const ExecutionProviderImpl* providerImpl,
        const bool graphSerializationEnabled,
        const std::vector<uint8_t>& isInputsUploadedByDmlEP,
        const std::unordered_map<uint32_t, uint32_t>* serializedGraphInputIndexToSubgraphInputIndex,
        const std::unordered_map<std::string_view, uint32_t>* serializedGraphLargeConstantNameToSubgraphInputIndex,
        const gsl::span<const std::string> subGraphInputArgNames,
        const std::unordered_map<std::string, std::pair<const ONNX_NAMESPACE::TensorProto*, bool>>& initializerNameToInitializerMap,
        onnxruntime::Graph& graph,
        _Out_ std::vector<bool>& inputsUsed,
        _Inout_ std::vector<DML_BUFFER_BINDING>& initInputBindings,
        _Inout_ std::vector<ComPtr<ID3D12Resource>>& nonOwnedGraphInputsFromInitializers,
        _Inout_ std::vector<ComPtr<ID3D12Resource>>& initializeResourceRefs,
        _Inout_opt_ std::vector<std::vector<std::byte>>* inputRawData)
    {

        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(subGraphInputArgNames.size());

        // Determine the last input which uses an initializer, so initializers can be freed incrementally
        // while processing each input in order.
        std::map<const onnx::TensorProto*, uint32_t> initializerToLastInputIndexMap;
        for (uint32_t i = 0; i < fusedNodeInputCount; i++)
        {
            auto iter = initializerNameToInitializerMap.find(subGraphInputArgNames[i]);
            if (iter != initializerNameToInitializerMap.end()) {
                initializerToLastInputIndexMap[iter->second.first] = i;
            }
        }

        // Walk through each graph edge and mark used inputs
        inputsUsed.assign(fusedNodeInputCount, false);
        for (auto it = serializedGraphInputIndexToSubgraphInputIndex->begin(); it != serializedGraphInputIndexToSubgraphInputIndex->end(); it++) {
            inputsUsed[it->second] = true;
        }
        for (auto it = serializedGraphLargeConstantNameToSubgraphInputIndex->begin(); it != serializedGraphLargeConstantNameToSubgraphInputIndex->end(); it++) {
            inputsUsed[it->second] = true;
        }

        std::wstring modelName;
        if (graphSerializationEnabled)
        {
            modelName = GetModelName(graph.ModelPath());
        }

        for (uint32_t i = 0; i < initInputBindings.size(); i++)
        {
            bool isInitializerAlreadyRemoved = false;
            // If the input isn't actually used by the graph, nothing ever needs to be bound (either for
            // initialization or execution). So just throw away the transferred initializer and skip this input.
            if (!inputsUsed[i])
            {
                auto iter = initializerNameToInitializerMap.find(subGraphInputArgNames[i]);
                if(iter != initializerNameToInitializerMap.end() && iter->second.second)
                {
                    graph.RemoveInitializedTensor(subGraphInputArgNames[i]);
                }

                if (inputRawData)
                {
                    inputRawData->push_back(std::vector<std::byte>());
                }

                continue;
            }

            // Look for the initializer among those transferred from the graph during partitioning
            auto iter = initializerNameToInitializerMap.find(subGraphInputArgNames[i]);
            if (iter != initializerNameToInitializerMap.end())
            {
                auto* initializer = iter->second.first;
                auto [unpackedTensor, unpackedExternalTensor, tensorPtr, tensorByteSize] = UnpackInitializer(graph, initializer);

                if (initializer->data_location() != onnx::TensorProto_DataLocation_EXTERNAL && !initializer->has_raw_data())
                {
                    // Free the initializer if this is the last usage of it.
                    if (initializerToLastInputIndexMap[initializer] == i)
                    {
                        if (iter->second.second)
                        {
                            graph.RemoveInitializedTensor(subGraphInputArgNames[i]);
                            isInitializerAlreadyRemoved = true;
                        }
                    }
                }

                // Tensor sizes in DML must be a multiple of 4 bytes large.
                tensorByteSize = AlignToPow2<size_t>(tensorByteSize, 4);
                if(graphSerializationEnabled)
                {
                    WriteToFile(modelName, ConvertToWString(iter->first) + L".bin", reinterpret_cast<uint8_t*>(tensorPtr), tensorByteSize);
                }

                if (inputRawData)
                {
                    inputRawData->push_back(std::vector<std::byte>(tensorPtr, tensorPtr + tensorByteSize));
                }

                if (!isInputsUploadedByDmlEP[i])
                {
                    // Store the resource to use during execution
                    ComPtr<ID3D12Resource> defaultBuffer = CreateResource(providerImpl, tensorPtr, tensorByteSize);
                    nonOwnedGraphInputsFromInitializers[i] = defaultBuffer;
                    initializeResourceRefs.push_back(std::move(defaultBuffer));
                }
                else
                {
                    ComPtr<ID3D12Resource> initializeInputBuffer;

                    if (!providerImpl->CustomHeapsSupported())
                    {
                        initializeInputBuffer = CreateResource(providerImpl, tensorPtr, tensorByteSize);
                    }
                    else
                    {
                        initializeInputBuffer = CreateCpuResource(providerImpl, tensorPtr, tensorByteSize);
                    }

                    // Set the binding for operator initialization to the buffer
                    initInputBindings[i].Buffer = initializeInputBuffer.Get();
                    initInputBindings[i].SizeInBytes = tensorByteSize;
                    initializeResourceRefs.push_back(std::move(initializeInputBuffer));
                }

                // Free the initializer if this is the last usage of it.
                if (!isInitializerAlreadyRemoved && initializerToLastInputIndexMap[initializer] == i)
                {
                    if (iter->second.second)
                    {
                        graph.RemoveInitializedTensor(subGraphInputArgNames[i]);
                    }
                }
            }
            else if (inputRawData)
            {
                inputRawData->push_back(std::vector<std::byte>());
            }
        }
    }

    std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>>
    GetInitializerToPartitionMap(
        const onnxruntime::GraphViewer& graph,
        gsl::span<std::unique_ptr<GraphPartition>> partitions
    )
    {
        std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>> initializerPartitionMap;
        for (uint32_t partitionIndex = 0; partitionIndex < gsl::narrow_cast<uint32_t>(partitions.size()); ++partitionIndex)
        {
            auto& partition = partitions[partitionIndex];

            // Skip partitions which have been merged into other partitions
            if (partition->GetRootMergedPartition() != partition.get())
            {
                continue;
            }

            for (const std::string& input : partition->GetInputs())
            {
                const onnx::TensorProto* tensor = nullptr;
                if (graph.GetInitializedTensor(input, tensor))
                {
                    initializerPartitionMap[tensor].push_back(partitionIndex);
                }
            }
        }

        return initializerPartitionMap;
    }

    inline uint32_t GetConstantNodeGraphInputIndex(
        const std::string& constantName,
        const std::unordered_map<std::string_view, uint32_t>* serializedGraphConstantNameToMainGraphInputIndex,
        uint32_t& graphMaxInputIndex,
        std::unordered_map<std::string_view, uint32_t>& localConstantNameToIndexMap)
    {
        if (serializedGraphConstantNameToMainGraphInputIndex == nullptr)
        {
            if (localConstantNameToIndexMap.find(constantName) == localConstantNameToIndexMap.end())
            {
                localConstantNameToIndexMap[constantName] = ++graphMaxInputIndex;
            }
            return localConstantNameToIndexMap[constantName];
        }
        else
        {
            graphMaxInputIndex = std::max(graphMaxInputIndex, serializedGraphConstantNameToMainGraphInputIndex->at(constantName));
            return serializedGraphConstantNameToMainGraphInputIndex->at(constantName);
        }
    }

    template <size_t AllocatorSize>
    void ConvertGraphDesc(
        const Dml::GraphDescBuilder::GraphDesc& graphDesc,
        const uint32_t inputCount,
        const uint32_t outputCount,
        IDMLDevice* device,
        StackAllocator<AllocatorSize>& allocator,
        const std::unordered_map<uint32_t, uint32_t>* serializedGraphInputIndexToSubgraphInputIndex,
        const std::unordered_map<std::string_view, uint32_t>* serializedGraphLargeConstantNameToSubgraphInputIndex,
        _Out_ DML_GRAPH_DESC& dmlGraphDesc,
        _Inout_ std::vector<ComPtr<IDMLOperator>>& dmlOperators,
        _Inout_ std::vector<DML_GRAPH_NODE_DESC>& dmlGraphNodes,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlInputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlOutputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlIntermediateEdges)
    {
        std::unordered_map<uint32_t, uint32_t> oldNodeIndexToNewNodeIndexMap;
        for (uint32_t index = 0; index < static_cast<uint32_t>(graphDesc.Nodes.size()); index++)
        {
            const DmlSerializedGraphNode& node = graphDesc.Nodes[index];
            if (std::holds_alternative<AbstractOperatorDesc>(node.Desc))
            {
                oldNodeIndexToNewNodeIndexMap[index] = static_cast<uint32_t>(dmlGraphNodes.size());
                DML_OPERATOR_DESC dmlDesc = SchemaHelpers::ConvertOperatorDesc<AllocatorSize>(std::get<AbstractOperatorDesc>(node.Desc), &allocator);
                ComPtr<IDMLOperator> op;
                ORT_THROW_IF_FAILED(device->CreateOperator(&dmlDesc, IID_PPV_ARGS(&op)));
                dmlOperators.push_back(op);
                DML_OPERATOR_GRAPH_NODE_DESC* dmlOperatorGraphNode = allocator.template Allocate<DML_OPERATOR_GRAPH_NODE_DESC>();
                dmlOperatorGraphNode->Name = node.Name.data();
                dmlOperatorGraphNode->Operator = op.Get();
                dmlGraphNodes.push_back(DML_GRAPH_NODE_DESC{DML_GRAPH_NODE_TYPE_OPERATOR, dmlOperatorGraphNode});
            }
            else
            {
                auto& constantNodeVariant = std::get<DmlSerializedGraphNodeConstantVariant>(node.Desc);
                if (std::holds_alternative<ConstantData>(constantNodeVariant))
                {
                    oldNodeIndexToNewNodeIndexMap[index] = static_cast<uint32_t>(dmlGraphNodes.size());

                    auto& constantData = std::get<ConstantData>(constantNodeVariant);
                    
                    DML_CONSTANT_DATA_GRAPH_NODE_DESC* constantNode = allocator.template Allocate<DML_CONSTANT_DATA_GRAPH_NODE_DESC>();
                    constantNode->Name = node.Name.data();
                    constantNode->DataSize = constantData.dataSize;
                    constantNode->Data = constantData.data;
                    dmlGraphNodes.push_back(DML_GRAPH_NODE_DESC{DML_GRAPH_NODE_TYPE_CONSTANT, constantNode});
                }
            }
        }

        uint32_t graphMaxInputIndex = 0;

        for (size_t i = 0; i < graphDesc.InputEdges.size(); ++i)
        {
            DML_INPUT_GRAPH_EDGE_DESC* edge = allocator.template Allocate<DML_INPUT_GRAPH_EDGE_DESC>();
            // 1. If serializedGraphInputIndexToMainGraphInputIndex is not null:
            //      then use the corresponding main graph input index, because the caller will use corresponding
            //      main graph input index for extracting the actual input tensor from the main graph and
            //      the caller does not own the creation of dml bindings directly.
            //      Use Case: When the caller is ORT (DML EP) or DmlEngine.
            //
            // 2. If serializedGraphInputIndexToMainGraphInputIndex is null:
            //      then assign the sequential graph input index, because it owns the creation of dml bindings
            //      directly.
            edge->GraphInputIndex = serializedGraphInputIndexToSubgraphInputIndex == nullptr ?
                graphDesc.InputEdges[i].GraphInputIndex :
                serializedGraphInputIndexToSubgraphInputIndex->at(graphDesc.InputEdges[i].GraphInputIndex);
            edge->ToNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.InputEdges[i].ToNodeIndex];
            edge->ToNodeInputIndex = graphDesc.InputEdges[i].ToNodeInputIndex;
            edge->Name = graphDesc.InputEdges[i].Name.data();

            graphMaxInputIndex = std::max(graphMaxInputIndex, edge->GraphInputIndex);
            dmlInputEdges.push_back(DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INPUT, edge});
        }

        for (size_t i = 0; i < graphDesc.OutputEdges.size(); ++i)
        {
            DML_OUTPUT_GRAPH_EDGE_DESC* edge = allocator.template Allocate<DML_OUTPUT_GRAPH_EDGE_DESC>();
            edge->GraphOutputIndex = graphDesc.OutputEdges[i].GraphOutputIndex;
            edge->FromNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.OutputEdges[i].FromNodeIndex];
            edge->FromNodeOutputIndex = graphDesc.OutputEdges[i].FromNodeOutputIndex;
            edge->Name = graphDesc.OutputEdges[i].Name.data();

            dmlOutputEdges.push_back(DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_OUTPUT, edge});
        }

        std::unordered_map<std::string_view, uint32_t> localConstantNameToIndexMap;
        for (uint32_t i = 0; i < static_cast<uint32_t>(graphDesc.IntermediateEdges.size()); ++i)
        {
            DmlSerializedGraphNodeDescVariant descVariant = graphDesc.Nodes[graphDesc.IntermediateEdges[i].FromNodeIndex].Desc;
            bool isConstantEdge = std::holds_alternative<DmlSerializedGraphNodeConstantVariant>(descVariant);
            if (isConstantEdge)
            {
                auto& constantNodeVariant = std::get<DmlSerializedGraphNodeConstantVariant>(descVariant);
                if (std::holds_alternative<ConstantData>(constantNodeVariant))
                {
                    DML_INTERMEDIATE_GRAPH_EDGE_DESC* edge = allocator.template Allocate<DML_INTERMEDIATE_GRAPH_EDGE_DESC>();
                    edge->FromNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.IntermediateEdges[i].FromNodeIndex];
                    edge->FromNodeOutputIndex = graphDesc.IntermediateEdges[i].FromNodeOutputIndex;
                    edge->ToNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.IntermediateEdges[i].ToNodeIndex];
                    edge->ToNodeInputIndex = graphDesc.IntermediateEdges[i].ToNodeInputIndex;
                    edge->Name = graphDesc.IntermediateEdges[i].Name.data();
                    dmlIntermediateEdges.push_back(DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INTERMEDIATE, edge});
                }
                else
                {
                    const std::string& constantName = graphDesc.Nodes[graphDesc.IntermediateEdges[i].FromNodeIndex].Name;

                    DML_INPUT_GRAPH_EDGE_DESC* edge = allocator.template Allocate<DML_INPUT_GRAPH_EDGE_DESC>();
                    edge->GraphInputIndex = GetConstantNodeGraphInputIndex(
                        constantName,
                        serializedGraphLargeConstantNameToSubgraphInputIndex,
                        graphMaxInputIndex,
                        localConstantNameToIndexMap);
                    edge->ToNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.IntermediateEdges[i].ToNodeIndex];
                    edge->ToNodeInputIndex = graphDesc.IntermediateEdges[i].ToNodeInputIndex;
                    edge->Name = graphDesc.IntermediateEdges[i].Name.data();

                    dmlInputEdges.push_back({DML_GRAPH_EDGE_TYPE_INPUT, edge});
                }
            }
            else
            {
                DML_INTERMEDIATE_GRAPH_EDGE_DESC* edge = allocator.template Allocate<DML_INTERMEDIATE_GRAPH_EDGE_DESC>();
                edge->FromNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.IntermediateEdges[i].FromNodeIndex];
                edge->FromNodeOutputIndex = graphDesc.IntermediateEdges[i].FromNodeOutputIndex;
                edge->ToNodeIndex = oldNodeIndexToNewNodeIndexMap[graphDesc.IntermediateEdges[i].ToNodeIndex];
                edge->ToNodeInputIndex = graphDesc.IntermediateEdges[i].ToNodeInputIndex;
                edge->Name = graphDesc.IntermediateEdges[i].Name.data();
                dmlIntermediateEdges.push_back(DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INTERMEDIATE, edge});
            }
        }

        dmlGraphDesc.InputCount = inputCount;
        dmlGraphDesc.OutputCount = outputCount;
        dmlGraphDesc.NodeCount = gsl::narrow_cast<uint32_t>(dmlGraphNodes.size());
        dmlGraphDesc.Nodes = dmlGraphNodes.data();
        dmlGraphDesc.InputEdgeCount = gsl::narrow_cast<uint32_t>(dmlInputEdges.size());
        dmlGraphDesc.InputEdges = dmlInputEdges.data();
        dmlGraphDesc.OutputEdgeCount = gsl::narrow_cast<uint32_t>(dmlOutputEdges.size());
        dmlGraphDesc.OutputEdges = dmlOutputEdges.data();
        dmlGraphDesc.IntermediateEdgeCount = gsl::narrow_cast<uint32_t>(dmlIntermediateEdges.size());
        dmlGraphDesc.IntermediateEdges = dmlIntermediateEdges.data();
    }

    onnxruntime::IndexedSubGraph CreateIndexedSubGraph(
        GraphPartition* partition,
        uint32_t partitionIndex,
        const std::string& partitionKernelPrefix)
    {
        assert(partition->IsDmlGraphPartition());

        onnxruntime::IndexedSubGraph indexedSubGraph;
        // Create a definition for the node.  The name must be unique.
        auto def = std::make_unique<onnxruntime::IndexedSubGraph::MetaDef>();
        def->name = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_NAME_PREFIX + partitionKernelPrefix + std::to_string(partitionIndex);
        def->domain = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_DOMAIN;
        def->since_version = 1;
        def->inputs.insert(def->inputs.begin(), partition->GetInputs().begin(), partition->GetInputs().end());
        def->outputs.insert(def->outputs.begin(), partition->GetOutputs().begin(), partition->GetOutputs().end());

        indexedSubGraph.SetMetaDef(std::move(def));
        indexedSubGraph.nodes = std::move(partition->GetNodeIndices());

        return indexedSubGraph;
    }

    std::unordered_map<std::string, GraphNodeProperties> CreatePartitionNodePropsMap(
        const onnxruntime::Graph& graph,
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties>&& graphNodePropertyMap)
    {
        // Populate properties which will be passed to OpKernel for this graph via the function below
        std::unordered_map<std::string, GraphNodeProperties> partitionNodePropsMap;
        for (auto nodeIndex : indexedSubGraph.nodes)
        {
            const onnxruntime::Node* node = graph.GetNode(nodeIndex);

#ifdef PRINT_PARTITON_INFO
            printf("Partition %u\t%s\n", partitionIndex, GraphDescBuilder::GetUniqueNodeName(*node).c_str());
#endif
            partitionNodePropsMap.insert(std::make_pair(
                GraphDescBuilder::GetUniqueNodeName(*node), std::move(graphNodePropertyMap[node])));
        }

#ifdef PRINT_PARTITON_INFO
        printf("\n");
#endif

        return partitionNodePropsMap;
    }

    Microsoft::WRL::ComPtr<IDMLCompiledOperator> TryCreateCompiledOperator(
        const GraphDescBuilder::GraphDesc& graphDesc,
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        const ExecutionProviderImpl* providerImpl,
        const std::unordered_map<uint32_t, uint32_t>* serializedGraphInputIndexToSubgraphInputIndex,
        const std::unordered_map<std::string_view, uint32_t>* serializedGraphLargeConstantNameToSubgraphInputIndex)
    {
        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->inputs.size());
        const uint32_t fusedNodeOutputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->outputs.size());

        // convert DML EP GraphDesc into DML_GRAPH_DESC and create IDMLCompiledOperator
        ComPtr<IDMLDevice> device;
        ORT_THROW_IF_FAILED(providerImpl->GetDmlDevice(device.GetAddressOf()));

        StackAllocator<1024> allocator;
        DML_GRAPH_DESC dmlGraphDesc = {};
        std::vector<ComPtr<IDMLOperator>> dmlOperators;
        std::vector<DML_GRAPH_NODE_DESC> dmlGraphNodes;
        std::vector<DML_GRAPH_EDGE_DESC> dmlInputEdges;
        std::vector<DML_GRAPH_EDGE_DESC> dmlOutputEdges;
        std::vector<DML_GRAPH_EDGE_DESC> dmlIntermediateEdges;
        ConvertGraphDesc(
            graphDesc,
            fusedNodeInputCount,
            fusedNodeOutputCount,
            device.Get(),
            allocator,
            serializedGraphInputIndexToSubgraphInputIndex,
            serializedGraphLargeConstantNameToSubgraphInputIndex,
            dmlGraphDesc,
            dmlOperators,
            dmlGraphNodes,
            dmlInputEdges,
            dmlOutputEdges,
            dmlIntermediateEdges);

        DML_EXECUTION_FLAGS executionFlags = DML_EXECUTION_FLAG_NONE;
        if (graphDesc.reuseCommandList)
        {
            executionFlags |= DML_EXECUTION_FLAG_DESCRIPTORS_VOLATILE;
        }

        // Query DML execution provider to see if metacommands is enabled
        if (!providerImpl->MetacommandsEnabled())
        {
            executionFlags |= DML_EXECUTION_FLAG_DISABLE_META_COMMANDS;
        }


        ComPtr<IDMLDevice1> device1;
        ORT_THROW_IF_FAILED(device.As(&device1));

        ComPtr<IDMLCompiledOperator> compiledExecutionPlanOperator;
        ORT_THROW_IF_FAILED(device1->CompileGraph(
            &dmlGraphDesc,
            executionFlags,
            IID_PPV_ARGS(&compiledExecutionPlanOperator)));

        // UINT32_MAX is currently the maximum number of bytes allowed by D3D12 for the offset of a view over a resource
        if (compiledExecutionPlanOperator->GetBindingProperties().PersistentResourceSize > UINT32_MAX)
        {
            return nullptr;
        }

        return compiledExecutionPlanOperator;
    }

    void FusePartitionAndRegisterKernel(
        const uint32_t partitionIndex,
        onnxruntime::Graph& graph,
        onnxruntime::KernelRegistry* registryForPartitionKernels,
        const std::unordered_map<std::string, std::pair<const ONNX_NAMESPACE::TensorProto*, bool>>& initializerNameToInitializerMap,
        const ExecutionProviderImpl* providerImpl,
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        std::vector<uint8_t>&& isInputsUploadedByDmlEP,
        const GraphDescBuilder::GraphDesc& graphDesc,
        Microsoft::WRL::ComPtr<IDMLCompiledOperator> compiledExecutionPlanOperator,
        const bool graphSerializationEnabled,
        const std::unordered_map<uint32_t, uint32_t>* serializedGraphInputIndexToSubgraphInputIndex,
        const std::unordered_map<std::string_view, uint32_t>* serializedGraphLargeConstantNameToSubgraphInputIndex)
    {
      if (graphSerializationEnabled)
      {

        const std::wstring modelName = GetModelName(graph.ModelPath());
        auto buffer = SerializeDmlGraph(graphDesc);

        const std::wstring partitionName =
            L"Partition_" +
            std::to_wstring(partitionIndex) +
            L".bin";
        WriteToFile(modelName, partitionName, buffer.data(), buffer.size());

        std::vector<std::unique_ptr<std::byte[]>> rawData;
        DmlSerializedGraphDesc deserializedGraphDesc = DeserializeDmlGraph(buffer.data(), rawData);
        GraphDescBuilder::GraphDesc deserializedDmlGraphDesc = {};
        deserializedDmlGraphDesc.InputCount = deserializedGraphDesc.InputCount;
        deserializedDmlGraphDesc.InputEdges = std::move(deserializedGraphDesc.InputEdges);
        deserializedDmlGraphDesc.IntermediateEdges = std::move(deserializedGraphDesc.IntermediateEdges);
        deserializedDmlGraphDesc.Nodes = std::move(deserializedGraphDesc.Nodes);
        deserializedDmlGraphDesc.OutputCount = deserializedGraphDesc.OutputCount;
        deserializedDmlGraphDesc.OutputEdges = std::move(deserializedGraphDesc.OutputEdges);
        deserializedDmlGraphDesc.reuseCommandList = graphDesc.reuseCommandList;
        deserializedDmlGraphDesc.outputShapes = graphDesc.outputShapes;

        compiledExecutionPlanOperator = DmlGraphFusionHelper::TryCreateCompiledOperator(
                        deserializedDmlGraphDesc,
                        indexedSubGraph,
                        providerImpl,
                        serializedGraphInputIndexToSubgraphInputIndex,
                        serializedGraphLargeConstantNameToSubgraphInputIndex);
      }

        auto& fusedNode = graph.BeginFuseSubGraph(indexedSubGraph, indexedSubGraph.GetMetaDef()->name);
        fusedNode.SetExecutionProviderType(onnxruntime::kDmlExecutionProvider);

        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->inputs.size());

        // Populate input bindings for operator initialization
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> initializeResourceRefs; // For lifetime control
        std::vector<DML_BUFFER_BINDING> initInputBindings(fusedNodeInputCount);
        std::vector<ComPtr<ID3D12Resource>> nonOwnedGraphInputsFromInitializers(fusedNodeInputCount);

        std::vector<bool> inputsUsed;
        ProcessInputData(
            providerImpl,
            graphSerializationEnabled,
            isInputsUploadedByDmlEP,
            serializedGraphInputIndexToSubgraphInputIndex,
            serializedGraphLargeConstantNameToSubgraphInputIndex,
            indexedSubGraph.GetMetaDef()->inputs,
            initializerNameToInitializerMap,
            graph,
            inputsUsed,
            initInputBindings,
            nonOwnedGraphInputsFromInitializers,
            initializeResourceRefs,
            nullptr);

        // lamda captures for the kernel registration
        Windows::AI::MachineLearning::Adapter::EdgeShapes outputShapes;
        ORT_THROW_HR_IF(E_UNEXPECTED, !TryGetStaticOutputShapes(fusedNode, outputShapes));
        bool resuableCommandList = graphDesc.reuseCommandList;
        auto fused_kernel_func = [compiledExecutionPlanOperator,
                                  outputShapes,
                                  resuableCommandList,
                                  nonOwnedGraphInputsFromInitializers,
                                  initializeResourceRefs,
                                  initInputBindings,
                                  isInputsUploadedByDmlEP = std::move(isInputsUploadedByDmlEP),
                                  inputsUsed = std::move(inputsUsed)]
                    (onnxruntime::FuncManager& func_mgr, const onnxruntime::OpKernelInfo& info, std::unique_ptr<onnxruntime::OpKernel>& out) mutable ->onnxruntime::Status
        {
            out.reset(CreateFusedGraphKernel(info,
                                             compiledExecutionPlanOperator,
                                             outputShapes,
                                             resuableCommandList,
                                             nonOwnedGraphInputsFromInitializers,
                                             initializeResourceRefs,
                                             initInputBindings,
                                             std::move(isInputsUploadedByDmlEP),
                                             std::move(inputsUsed)));
            return Status::OK();
        };

        // build the kernel definition on the fly, and register it to the fused_kernel_regisitry.
        onnxruntime::KernelDefBuilder builder;
        builder.SetName(indexedSubGraph.GetMetaDef()->name)
            .SetDomain(indexedSubGraph.GetMetaDef()->domain)
            .SinceVersion(indexedSubGraph.GetMetaDef()->since_version)
            .Provider(onnxruntime::kDmlExecutionProvider);
        ORT_THROW_IF_ERROR(registryForPartitionKernels->Register(builder, fused_kernel_func));

        graph.FinalizeFuseSubGraph(indexedSubGraph, fusedNode);
    }

    void RegisterDynamicKernel(
        onnxruntime::Graph& graph,
        onnxruntime::KernelRegistry* registryForPartitionKernels,
        const ExecutionProviderImpl* providerImpl,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties> graphNodePropertyMap,
        const std::unordered_set<std::string>& dynamicCpuInputMap,
        std::shared_ptr<const onnxruntime::IndexedSubGraph> indexedSubGraph,
        std::unordered_map<std::string, std::pair<const ONNX_NAMESPACE::TensorProto*, bool>>&& isInitializerTransferable)
    {
        struct NodeInfo
        {
            std::string name;
            std::string opType;
            std::string description;
            std::string domain;
            onnxruntime::NodeAttributes attributes;
            std::vector<onnxruntime::NodeArg*> inputDefPointers;
            std::vector<onnxruntime::NodeArg*> outputDefPointers;
        };

        auto partitionNodePropsMap = DmlGraphFusionHelper::CreatePartitionNodePropsMap(
            graph,
            *indexedSubGraph,
            std::move(graphNodePropertyMap));

        auto modelPath = graph.ModelPath();

        const gsl::span<const std::string> subGraphInputArgNames = indexedSubGraph->GetMetaDef()->inputs;
        const gsl::span<const std::string> subGraphOutputArgNames = indexedSubGraph->GetMetaDef()->outputs;

        std::vector<NodeInfo> nodesInfo;
        nodesInfo.reserve(indexedSubGraph->nodes.size());

        std::vector<const onnxruntime::NodeArg*> subgraphInputs;
        subgraphInputs.reserve(subGraphInputArgNames.size());

        std::vector<const onnxruntime::NodeArg*> subgraphOutputs;
        subgraphOutputs.reserve(subGraphOutputArgNames.size());

        std::vector<onnxruntime::NodeAttributes> nodeAttributes;
        nodeAttributes.reserve(indexedSubGraph->nodes.size());

        std::vector<std::shared_ptr<onnxruntime::NodeArg>> intermediateNodeArgs;

        for (size_t sortedNodeIndex : indexedSubGraph->nodes)
        {
            auto node = graph.GetNode(sortedNodeIndex);

            nodeAttributes.push_back(node->GetAttributes());

            NodeInfo nodeInfo{};
            nodeInfo.name = node->Name();
            nodeInfo.opType = node->OpType();
            nodeInfo.description = node->Description();
            nodeInfo.domain = node->Domain();
            nodeInfo.attributes = node->GetAttributes();
            nodeInfo.inputDefPointers.reserve(node->InputDefs().size());
            nodeInfo.outputDefPointers.reserve(node->OutputDefs().size());

            for (const onnxruntime::NodeArg* inputDef : node->InputDefs())
            {
                intermediateNodeArgs.emplace_back(std::make_shared<onnxruntime::NodeArg>(inputDef->Name(), inputDef->TypeAsProto()));
                nodeInfo.inputDefPointers.push_back(intermediateNodeArgs.back().get());
            }

            for (const onnxruntime::NodeArg* outputDef : node->OutputDefs())
            {
                intermediateNodeArgs.emplace_back(std::make_shared<onnxruntime::NodeArg>(outputDef->Name(), outputDef->TypeAsProto()));
                nodeInfo.outputDefPointers.push_back(intermediateNodeArgs.back().get());
            }

            nodesInfo.push_back(std::move(nodeInfo));
        }

        for (const std::string& graphInputName : subGraphInputArgNames)
        {
            subgraphInputs.push_back(graph.GetNodeArg(graphInputName));
        }

        for (const std::string& graphOutputName : subGraphOutputArgNames)
        {
            subgraphOutputs.push_back(graph.GetNodeArg(graphOutputName));
        }

        // We need to keep the initializers alive since they will be freed once the nodes are removed from the graph
        std::vector<ONNX_NAMESPACE::TensorProto> ownedInitializers;
        ownedInitializers.reserve(isInitializerTransferable.size());

        for (auto& kvp : isInitializerTransferable)
        {
            auto [unpackedTensor, unpackedExternalTensor, tensorPtr, tensorByteSize] = UnpackInitializer(graph, kvp.second.first);

            ONNX_NAMESPACE::TensorProto tensorProto;
            tensorProto.set_data_type(kvp.second.first->data_type());
            tensorProto.set_raw_data(tensorPtr, tensorByteSize);
            tensorProto.set_name(kvp.second.first->name());

            for (int i = 0; i < kvp.second.first->dims_size(); ++i)
            {
                tensorProto.add_dims(kvp.second.first->dims(i));
            }
            ownedInitializers.push_back(std::move(tensorProto));
            kvp.second.first = &ownedInitializers.back();
        }

        // lamda captures for the kernel registration
        auto fused_kernel_func = [
            indexedSubGraph,
            &modelPath,
            nodesInfo = std::move(nodesInfo),
            intermediateNodeArgs = std::move(intermediateNodeArgs),
            subgraphInputs = std::move(subgraphInputs),
            subgraphOutputs = std::move(subgraphOutputs),
            partitionNodePropsMap = std::move(partitionNodePropsMap),
            ownedInitializers = std::move(ownedInitializers)] (onnxruntime::FuncManager& func_mgr, const onnxruntime::OpKernelInfo& info, std::unique_ptr<onnxruntime::OpKernel>& out) mutable ->onnxruntime::Status
        {
            std::vector<std::shared_ptr<onnxruntime::Node>> subgraphNodes;
            subgraphNodes.reserve(nodesInfo.size());

            for (const NodeInfo& nodeInfo : nodesInfo)
            {
                subgraphNodes.emplace_back(std::make_shared<onnxruntime::Node>(
                    nodeInfo.name,
                    nodeInfo.opType,
                    nodeInfo.description,
                    nodeInfo.inputDefPointers,
                    nodeInfo.outputDefPointers,
                    &nodeInfo.attributes,
                    nodeInfo.domain));
            }

            out.reset(CreateRuntimeFusedGraphKernel(
                info,
                indexedSubGraph,
                modelPath,
                std::move(subgraphNodes),
                std::move(subgraphInputs),
                std::move(subgraphOutputs),
                std::move(intermediateNodeArgs),
                std::move(partitionNodePropsMap),
                std::move(ownedInitializers)));
            return Status::OK();
        };

        // build the kernel definition on the fly, and register it to the fused_kernel_regisitry.
        onnxruntime::KernelDefBuilder builder;
        builder.SetName(indexedSubGraph->GetMetaDef()->name)
            .SetDomain(indexedSubGraph->GetMetaDef()->domain)
            .SinceVersion(indexedSubGraph->GetMetaDef()->since_version)
            .Provider(onnxruntime::kDmlExecutionProvider);

        // Force the CPU inputs to be allocated on the CPU
        for (int i = 0; i < subGraphInputArgNames.size(); ++i)
        {
            if (dynamicCpuInputMap.find(subGraphInputArgNames[i]) != dynamicCpuInputMap.end())
            {
                builder.InputMemoryType(OrtMemTypeCPUInput, i);
            }
        }

        ORT_THROW_IF_ERROR(registryForPartitionKernels->Register(builder, fused_kernel_func));

        auto& fusedNode = graph.BeginFuseSubGraph(*indexedSubGraph, indexedSubGraph->GetMetaDef()->name);
        fusedNode.SetExecutionProviderType(onnxruntime::kDmlExecutionProvider);

        graph.FinalizeFuseSubGraph(*indexedSubGraph, fusedNode);
    }
}
}
