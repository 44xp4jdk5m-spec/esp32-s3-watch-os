#pragma once
/* NOTE: local trim — only the operators used by the wake-on-raise model are
 * compiled/registered.  This keeps esp-dl's linked footprint small on this
 * memory-constrained watch (full list pulls in ~658 KB of operator kernels). */
#include "dl_module_conv.hpp"
#include "dl_module_flatten.hpp"
#include "dl_module_gemm.hpp"
#include "dl_module_identity.hpp"
#include "dl_module_lut.hpp"
#include "dl_module_max_pool.hpp"
#include "dl_module_requantize_linear.hpp"
#include "dl_module_relu.hpp"
#include "dl_module_reshape.hpp"
#include "dl_module_sigmoid.hpp"
#include "dl_module_transpose.hpp"
#include "fbs_loader.hpp"
#include <functional>
#include <iostream>
#include <malloc.h>
#include <map>
namespace dl {
namespace module {

/**
 * @brief Singleton class for registering modules.
 *
 */
class ModuleCreator {
public:
    using Creator = std::function<Module *(fbs::FbsModel *, std::string)>; ///< Module creator function type

    /**
     * @brief Get instance of ModuleCreator by this function. It is only safe method to get instance of ModuleCreator
     * because ModuleCreator is a singleton class.
     *
     * @return ModuleCreator instance pointer
     */
    static ModuleCreator *get_instance()
    {
        // This is thread safe for C++11, please refer to `Meyers' implementation of the Singleton pattern`
        static ModuleCreator instance;
        return &instance;
    }
    /**
     * @brief Register a module creator to the module creator map
     *        This function allows for the dynamic registration of new module types and their corresponding creator
     * functions at runtime. By associating the module type name with the creator function, the system can flexibly
     * create instances of various modules.
     *
     * @param op_type The module type name, used as the key in the map
     * @param creator The module creator function, used to create modules of a specific type
     */
    void register_module(const std::string &op_type, Creator creator) { ModuleCreator::creators[op_type] = creator; }

    /**
     * @brief Create module instance pointer
     *
     * @param fbs_model  Flatbuffer model pointer
     * @param op_type    Module/Operator type
     * @param name       Module name
     *
     * @return Module instance pointer
     */
    Module *create(fbs::FbsModel *fbs_model, const std::string &op_type, const std::string name)
    {
        this->register_dl_modules();

        if (creators.find(op_type) != creators.end()) {
            return creators[op_type](fbs_model, name);
        }
        return nullptr;
    }

    /**
     * @brief Pre-register the already implemented modules
     */
    void register_dl_modules()
    {
        if (creators.empty()) {
            this->register_module("Conv", Conv::deserialize);
            this->register_module("Sigmoid", Sigmoid::deserialize);
            this->register_module("Relu", Relu::deserialize);
            this->register_module("Gemm", Gemm::deserialize);
            this->register_module("QuantizeLinear", RequantizeLinear::deserialize);
            this->register_module("DequantizeLinear", RequantizeLinear::deserialize);
            this->register_module("RequantizeLinear", RequantizeLinear::deserialize);
            this->register_module("Flatten", Flatten::deserialize);
            this->register_module("Reshape", Reshape::deserialize);
            this->register_module("Transpose", Transpose::deserialize);
            this->register_module("MaxPool", MaxPool::deserialize);
            this->register_module("Gelu", LUT::deserialize);
            this->register_module("LUT", LUT::deserialize);
            this->register_module("Identity", Identity::deserialize);
        }
    }

    /**
     * @brief Print all modules has been registered
     */
    void print()
    {
        if (!creators.empty()) {
            for (auto it = creators.begin(); it != creators.end(); ++it) {
                printf("%s", (*it).first.c_str());
            }
        } else {
            printf("Create empty module\n");
        }
    }

    /**
     * @brief Clear all modules has been registered
     */
    void clear()
    {
        if (!creators.empty()) {
            std::map<std::string, Creator> temp;
            creators.swap(temp);
        }
    }

private:
    ModuleCreator() {}
    ~ModuleCreator() {}
    ModuleCreator(const ModuleCreator &) = delete;
    ModuleCreator &operator=(const ModuleCreator &) = delete;
    std::map<std::string, Creator> creators;
};

} // namespace module
} // namespace dl
