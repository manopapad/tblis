#ifndef _TBLIS_NODES_GEMM_HPP_
#define _TBLIS_NODES_GEMM_HPP_

#include "partm.hpp"
#include "packm.hpp"
#include "matrify.hpp"
#include "gemm_mkr.hpp"
#include "gemm_ukr.hpp"

namespace tblis
{

extern MemoryPool BuffersForA, BuffersForB, BuffersForScatter;

template <template <typename> class NodeType>
struct node_helper
{
    template <typename T>
    auto operator()(T& tree, ...) const -> decltype(node_helper<NodeType>()(tree.child))
    {
        return node_helper<NodeType>()(tree.child);
    }

    template <typename T>
    NodeType<T>& operator()(NodeType<T>& tree) const
    {
        return tree;
    }
};

template <template <typename> class NodeType, typename T>
auto node(T& tree) -> decltype(node_helper<NodeType>()(tree))
{
    return node_helper<NodeType>()(tree);
}

template <typename Child>
struct gemm
{
    Child child;

    template <typename T, typename MatrixA, typename MatrixB, typename MatrixC>
    void operator()(const communicator& comm, const config& cfg,
                    T alpha, MatrixA& A, MatrixB& B, T beta, MatrixC& C)
    {
        using namespace matrix_constants;

        const bool row_major = cfg.gemm_row_major.value<T>();

        len_type m = C.length(0);
        len_type n = C.length(1);
        len_type k = A.length(1);

        if (C.stride(!row_major) == 1)
        {
            /*
             * Compute C^T = B^T * A^T instead
             */
            std::swap(m, n);
            A.transpose();
            B.transpose();
            C.transpose();
        }

        if (comm.master()) flops += 2*m*n*k;

        int nt = comm.num_threads();
        auto tc = make_gemm_thread_config<T>(cfg, nt, m, n, k);

        communicator comm_nc =    comm.gang(TCI_EVENLY, tc.jc_nt);
        communicator comm_kc = comm_nc.gang(TCI_EVENLY,        1);
        communicator comm_mc = comm_kc.gang(TCI_EVENLY, tc.ic_nt);
        communicator comm_nr = comm_mc.gang(TCI_EVENLY, tc.jr_nt);
        communicator comm_mr = comm_nr.gang(TCI_EVENLY, tc.ir_nt);

        node<partition_gemm_nc>(child).subcomm = &comm_nc;
        node<partition_gemm_kc>(child).subcomm = &comm_kc;
        node<partition_gemm_mc>(child).subcomm = &comm_mc;
        node<partition_gemm_nr>(child).subcomm = &comm_nr;
        node<partition_gemm_mr>(child).subcomm = &comm_mr;

        if (C.stride(!row_major) == 1)
        {
            /*
             * Compute C^T = B^T * A^T instead
             */
            child(comm, cfg, alpha, B, A, beta, C);
        }
        else
        {
            child(comm, cfg, alpha, A, B, beta, C);
        }
    }
};

using GotoGEMM = gemm<
                   partition_gemm_nc<
                     partition_gemm_kc<
                       pack_b<BuffersForB,
                         partition_gemm_mc<
                           pack_a<BuffersForA,
                             partition_gemm_nr<
                               partition_gemm_mr<
                                 gemm_micro_kernel>>>>>>>>;

using GotoGEMM2 = gemm<
                    partition_gemm_nc<
                      partition_gemm_kc<
                        pack_b<BuffersForB,
                          partition_gemm_mc<
                            pack_a<BuffersForA,
                              gemm_macro_kernel>>>>>>;

using TensorGEMM = gemm<
                     partition_gemm_nc<
                       partition_gemm_kc<
                         matrify_and_pack_b<BuffersForB,
                           partition_gemm_mc<
                             matrify_and_pack_a<BuffersForA,
                               matrify_c<BuffersForScatter,
                                 partition_gemm_nr<
                                   partition_gemm_mr<
                                     gemm_micro_kernel>>>>>>>>>;

}

#endif
