//========================================================================================
// (C) (or copyright) 2022. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================
#ifndef SOLVERS_BICGSTAB_SOLVER_HPP_
#define SOLVERS_BICGSTAB_SOLVER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "interface/mesh_data.hpp"
#include "interface/meshblock_data.hpp"
#include "interface/state_descriptor.hpp"
#include "kokkos_abstraction.hpp"
#include "solvers/solver_utils.hpp"
#include "tasks/task_id.hpp"
#include "tasks/task_list.hpp"

namespace parthenon {

namespace solvers {

struct BiCGStabCounter {
  static int global_num_bicgstab_solvers;
};

template <typename SPType>
class BiCGStabSolver : BiCGStabCounter {
 public:
  BiCGStabSolver() = default;
  BiCGStabSolver(StateDescriptor *pkg, const Real error_tol_in,
                const SparseMatrixAccessor &sp)
    : error_tol(error_tol_in), sp_accessor(sp),
      max_iters(pkg->Param<int>("bicgstab_max_iterations")),
      check_interval(pkg->Param<int>("bicgstab_check_interval")),
      fail_flag(pkg->Param<bool>("bicgstab_abort_on_fail")),
      warn_flag(pkg->Param<bool>("bicgstab_warn_on_fail")) {
    Init(pkg);
  }
  std::vector<std::string> SolverState() const {
    return std::vector<std::string>(
      {spm_name, rhs_name, res, res0, vk, pk, tk}
    );
  }
  std::string label() const {
    std::string lab;
    for (const auto &s : SolverState())
      lab += s;
    return lab;
  }

  TaskID CreateTaskList(const TaskID &begin, const int i, TaskRegion &tr,
                        std::shared_ptr<MeshData<Real>> md,
                        std::shared_ptr<MeshData<Real>> mout) {
    auto &solver = tr[i].AddIteration(solver_name);
    solver.SetMaxIterations(max_iters);
    solver.SetCheckInterval(check_interval);
    solver.SetFailWithMaxIterations(fail_flag);
    solver.SetWarnWithMaxIterations(warn_flag);
    return createTaskList(begin, i, tr, solver, md, mout);
  }

 private:
  void Init(StateDescriptor *pkg) {
    // create vectors used internally by the solver
    spm_name = pkg->Param<std::string>("spm_name");
    sol_name = pkg->Param<std::string>("sol_name");
    rhs_name = pkg->Param<std::string>("rhs_name");

    const std::string bicg_id(std::to_string(global_num_bicgstab_solvers));
    solver_name = "internal_bicgstab_" + bicg_id;
    res0 = "res_0" + bicg_id;
    vk = "vk" + bicg_id;
    tk = "tk" + bicg_id;
    auto meta = Metadata({Metadata::Cell, Metadata::OneCopy});
    pkg->AddField(res0, meta);
    pkg->AddField(vk, meta);
    pkg->AddField(tk, meta);

    res = "res" + bicg_id;
    pk = "pk" + bicg_id;
    meta = Metadata({Metadata::Cell, Metadata::OneCopy, Metadata::FillGhost});
    pkg->AddField(pk, meta);
    pkg->AddField(res, meta);

    global_num_bicgstab_solvers++;
  }

  TaskID CreateTaskList(const TaskID &begin, const int i, TaskRegion &tr,
                        IterativeTasks &solver, std::shared_ptr<MeshData<Real> md,
                        std::shared_ptr<MeshData<Real>> mout) {
    using Solver_t = BiCGStabSolver<SPType>;
    TaskID none(0);
    TaskList &tl = tr[i];
    RegionCounter reg(solver_name);

    // initialize some shared state
    bicgstab_cntr = 0;
    global_res0.val = 0.0;
    global_res.val = 0.0;
    rhoi.val = 0.0;
    r0_dot_vk.val = 0.0;
    t_dot_s.val = 0.0;
    t_dot_t.val = 0.0;
  
    auto init_bicgstab = tl.AddTask(begin,
      &Solver_t::InitializeBiCGStab<MeshData<Real>>,
      this, md.get(), mout.get(), &global_res0.val);
    tr.AddRegionalDependencies(reg.ID(), i, init_bicgstab);
    // global reduction for initial residual 
    auto start_global_res0 = (i == 0 ?
      tl.AddTask(init_bicgstab, &AllReduce<Real>::StartReduce, &global_res0, MPI_SUM) :
      init_bicgstab);
    auto finish_global_res0 = tl.AddTask(start_global_res0,
      &AllReduce<Real>::CheckReduce, &global_res0);

    // 1. \hat{r}_0 \cdot r_{i-1}
    auto get_rhoi = solver.AddTask(init_bicgstab, &Solver_t::DotProduct, this,
      res0, res, &rhoi.val);
    tr.AddRegionalDependencies(reg.ID(), i, get_rhoi);

    // global reduction for rhoi
    auto start_global_rhoi = (i == 0 ?
      solver.AddTask(get_rhoi, &AllReduce<Real>::StartReduce, &rhoi, MPI_SUM) :
      get_rhoi);
    auto finish_global_rhoi = solver.AddTask(start_global_rhoi,
      &AllReduce<Real>::CheckReduce, &rhoi);

    // 2. \beta = (rho_i/rho_{i-1}) (\alpha / \omega_{i-1})
    // 3. p_i = r_{i-1} + \beta (p_{i-1} - \omega_{i-1} v_{i-1})
    auto update_pk = solver.AddTask(finish_global_rhoi, &Solver_t::Compute_pk, this,
      md.get());

    // ghost exchange for pk
    auto start_recv1 = solver.AddTask(none, &MeshData<Real>::StartReceiving, md.get(),
                                     BoundaryCommSubset::all);
    auto send1 =
        solver.AddTask(update_pk, parthenon::cell_centered_bvars::SendBoundaryBuffers, md);
    auto recv1 = solver.AddTask(
        start_recv1, parthenon::cell_centered_bvars::ReceiveBoundaryBuffers, md);
    auto setb1 =
        solver.AddTask(recv1 | update_pk, parthenon::cell_centered_bvars::SetBoundaries, md);
    auto clear1 = solver.AddTask(send1 | setb1, &MeshData<Real>::ClearBoundary, md.get(),
                                BoundaryCommSubset::all);

    // 4. v = A p
    auto get_v = solver.AddTask(clear1, &Solver_t::MatVec, this, md.get(), pk, vk);

    // 5. alpha = rho_i / (\hat{r}_0 \cdot v_i)
    auto get_r0dotv = solver.AddTask(get_v, &Solver_t::DotProduct, this,
      res0, vk, &r0_dot_vk.val);
    tr.AddRegionalDependencies(reg.ID(), i, get_r0dotv);
    auto start_global_r0dotv = (i == 0 ?
      solver.AddTask(get_r0dotv, &AllReduce<Real>::StartReduce, &r0_dot_vk, MPI_SUM) :
      get_rhoi);
    auto finish_global_r0dotv = solver.AddTask(start_global_r0dotv,
      &AllReduce<Real>::CheckReduce, &r0_dot_vk);
    // alpha is actually updated in this next task

    // 6. h = x_{i-1} + alpha p
    auto get_h = solver.AddTask(finish_global_r0dotv, &Solver_t::Update_h, this,
      md.get(), mout.get());

    // 7. check for convergence


    // 8. s = r_{i-1} - alpha v
    auto get_s = solver.AddTask(finish_global_r0dotv, &Solver_t::Update_s, this,
      md.get());

    // ghost exchange for sk
    auto start_recv2 = solver.AddTask(clear1, &MeshData<Real>::StartReceiving, md.get(),
                                     BoundaryCommSubset::all);
    auto send2 =
        solver.AddTask(get_s, parthenon::cell_centered_bvars::SendBoundaryBuffers, md);
    auto recv1 = solver.AddTask(
        start_recv2, parthenon::cell_centered_bvars::ReceiveBoundaryBuffers, md);
    auto setb2 =
        solver.AddTask(recv2 | get_s, parthenon::cell_centered_bvars::SetBoundaries, md);
    auto clear2 = solver.AddTask(send1 | setb1, &MeshData<Real>::ClearBoundary, md.get(),
                                BoundaryCommSubset::all);

    // 9. t = A s
    auto get_t = solver.AddTask(clear2, &Solver_t::MatVec, this, md.get(),
      res, tk);

    // 10. omega = (t \cdot s) / (t \cdot t)
    auto get_tdots = solver.AddTask(get_t, &Solver_t::OmegaDotProd, this,
      &t_dot_s.val, &t_dot_t.val);
    tr.AddRegionalDependencies(reg.ID(), i, get_tdots);
    auto start_global_tdots = (i == 0 ?
      solver.AddTask(get_tdots, &AllReduce<Real>::StartReduce, &t_dot_s, MPI_SUM) :
      get_tdots);
    auto finish_global_r0dotv = solver.AddTask(start_global_tdots,
      &AllReduce<Real>::CheckReduce, &t_dot_s);
    auto start_global_tdott = (i == 0 ?
      solver.AddTask(get_tdots, &AllReduce<Real>::StartReduce, &t_dot_t, MPI_SUM) :
      get_tdots);
    auto finish_global_tdott = solver.AddTask(start_global_tdott,
      &AllReduce<Real>::CheckReduce, &t_dot_t);
    // omega is actually updated in this next task

    // 11. update x and residual
    auto update_x = solver.AddTask(finish_global_tdots | finish_global_tdott,
      &Solver_t::Update_x_res, this, md.get(), mout.get(), &global_res.val);
    tr.AddRegionalDependencies(reg.ID(), i, update_x);
    auto start_global_res = (i == 0 ?
      solver.AddTask(update_x, &AllReduce<Real>::StartReduce, &global_res, MPI_SUM) :
      get_tdots);
    auto finish_global_res = solver.AddTask(start_global_res,
      &AllReduce<Real>::CheckReduce, &global_res);

    // 12. check for convergence
    auto check = solver.SetCompletionTask(finish_global_res, &Solver_t::CheckConvergence, this, i, true)
    tr.AddGlobalDependencies(reg.ID(), i, check);

    return check;
  }

  template <typename T>  
  TaskStatus InitializeBiCGStab(T *u, T *du, Real *gres0) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    PackIndexMap imap;
    std::vector<std::string> vars({res,res0,vk,pk,rhs_name});
    const auto &v = u->PackVariables(vars, imap);
    const int ires = imap[res].first;
    const int ires0 = imap[res0].first;
    const int ivk = imap[vk].first;
    const int ipk = imap[pk].first;
    const int irhs = imap[rhs_name].first;

    const auto &dv = du->PackVariables(std::vector<std::string>({sol_name}));

    rhoi_old = 1.0;
    alpha = 1.0;
    omega = 1.0;

    Real err(0);
    par_reduce(DEFAULT_LOOP_PATTERN, "initialize bicgstab", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lerr) {
        v(b, ires, k, j, i) = v(b, irhs, k, j, i);
        v(b, ires0, k, j, i) = v(b, irhs, k, j, i);
        lerr += v(b, irhs, k, j, i);
        v(b, ivk, k, j, i) = 0.0;
        v(b, ipk, k, j, i) = 0.0;
        // initialize guess for solution to zero
        dv(b, 0, k, j, i) = 0.0;
      }, Kokkos::Sum<Real>(err));
    *gres0 += err; 
    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus DotProduct(T *u, const std::string &vec1, const std::string &vec2,
                        Real *reduce_sum) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    auto &v = u->PackVariables(std::vector<std::string>({vec1,vec2}));

    Real gsum(0);
    par_reduce(loop_pattern_mdrange_tag, "DotProduct", DevExecSpace(), 0, v.GetDim(5) - 1,
      kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lsum) {
        lsum += v(b, 0, k, j, i) * v(b, 1, k, j, i);
      }, Kokkos::Sum<Real>(gsum));

    *reduce_sum += gsum;
    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus Compute_pk(T *u) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    PackIndexMap imap;
    auto &v = u->PackVariables(std::vector<std::string>({pk, res, vk}), imap);
    const int ipk = imap[pk].first;
    const int ires = imap[res].first;
    const int ivk = imap[vk].first;

    const Real beta = (rhoi.val/rhoi_old) * (alpha / omega);
    rhoi_old = rhoi.val;
    const Real w = omega;

    par_for(DEFAULT_LOOP_PATTERN, "compute pk", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i) {
        v(b, ipk, k, j, i) = v(b, ires, k, j, i)
                           + beta * (v(b, ipk, k, j, i) - w * v(b, ivk, k, j, i));
      });
    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus MatVec(T *u, const std::string &in_vec, const std::string &out_vec) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    PackIndexMap imap;
    auto &v = u->PackVariables(std::vector<std::string>({in_vec, out_vec, spm_name}), imap);
    const int iin = imap[in_vec].first;
    const int iout = imap[out_vec].first;
    const int isp_lo = imap[spm_name].first;
    const int isp_hi = imap[spm_name].second;

    par_for(DEFAULT_LOOP_PATTERN, "MatVec", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i) {
        v(b, iout, k, j, i) = sp_accessor.MatVec(v, isp_lo, isp_hi, v, iin, b, k, j, i);
      });
    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus Update_h(T *u, T *du) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    auto &v = u->PackVariables(std::vector<std::string>({pk}));
    auto &dv = u->PackVariables(std::vector<std::string>({sol_name}));
    const Real a = rhoi.val / r0_dot_vk.val;
    par_for(DEFAULT_LOOP_PATTERN, "Update_h", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i) {
        dv(b, 0, k, j, i) += a * v(b, 0, k, j, i);
      });
    return TaskStatus::complete;
  }
  template <typename T>
  TaskStatus Update_s(T *u) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    PackIndexMap imap;  
    auto &v = u->PackVariables(std::vector<std::string>({res, vk}), imap);
    const int ires = imap[res].first;
    const int ivk = imap[vk].first;
    const Real a = rhoi.val / r0_dot_vk.val;
    par_for(DEFAULT_LOOP_PATTERN, "Update_s", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i) {
        v(b, ires, k, j, i) -= a * v(b, ivk, k, j, i);
      });
    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus OmegaDotProd(T *u, Real *t_dot_s, Real *t_dot_t) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    auto &v = u->PackVariables(std::vector<std::string>({tk,res}));

    // TODO(JCD): these should probably be merged
    Real ts_sum(0);
    par_reduce(loop_pattern_mdrange_tag, "tk dot sk", DevExecSpace(), 0, v.GetDim(5) - 1,
      kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lsum) {
        lsum += v(b, 0, k, j, i) * v(b, 1, k, j, i);
      }, Kokkos::Sum<Real>(ts_sum));
    *t_dot_s += ts_sum;

    Real tt_sum(0);
    par_reduce(loop_pattern_mdrange_tag, "tk dot sk", DevExecSpace(), 0, v.GetDim(5) - 1,
      kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lsum) {
        lsum += v(b, 0, k, j, i) * v(b, 0, k, j, i);
      }, Kokkos::Sum<Real>(tt_sum));
    *t_dot_t += tt_sum;

    return TaskStatus::complete;
  }

  template <typename T>
  TaskStatus Update_x_res(T *u, T *du, Real *gres) {
    const auto &ib = u->GetBoundsI(IndexDomain::interior);
    const auto &jb = u->GetBoundsJ(IndexDomain::interior);
    const auto &kb = u->GetBoundsK(IndexDomain::interior);

    PackIndexMap imap;
    auto &v = u->PackVariables(std::vector<std::string>({res, tk}), imap);
    const int ires = imap[res].first;
    const int itk = imap[tk].first;
    auto &dv = u->PackVariables(std::vector<std::string>({sol_name}));
    omega = t_dot_s.val / t_dot_t.val;
    const Real w = omega;
    Real err(0);
    par_reduce(DEFAULT_LOOP_PATTERN, "Update_x", DevExecSpace(), 0,
      v.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lerr) {
        dv(b, 0, k, j, i) += w * v(b, ires, k, j, i);
        v(b, ires, k, j, i) -= w * v(b, itk, k, j, i);
        lerr += v(b, ires, k, j, i) * v(b, ires, k, j, i);
      }, Kokkos::Sum<Real>(err));
    *gres += err;
    return TaskStatus::complete;
  }

  TaskStatus CheckConvergence(const int &i, bool report) {
    if (i != 0) return TaskStatus::complete;
    bicgstab_cntr++;
    if (bicgstab_cntr == 1) global_res0.val = std::sqrt(global_res0.val);
    if (report) {
      global_res.val = std::sqrt(global_res.val);
      if (Globals::my_rank == 0) {
        std::cout << parthenon::Globals::my_rank << " its= " << bicgstab_cntr
                  << " relative res: " << global_res.val / global_res0.val << " absolute-res "
                  << global_res.val << " relerr-tol: " << error_tol << std::endl
                  << std::flush;
      }
    }
    bool converged = global_res.val / global_res0.val < error_tol;
    global_res.val = 0.0;
    rhoi.val = 0.0;
    r0_dot_vk.val = 0.0;
    t_dot_s.val = 0.0;
    t_dot_t.val = 0.0;
    return converged ? TaskStatus::complete : TaskStatus::iterate;
  }

  Real error_tol;
  SparseMatrixAccessor sp_accessor;
  int max_iters, check_interval, bicgstab_cntr;
  bool fail_flag, warn_flag;
  std::string spm_name, sol_name, rhs_name, res, res0, vk, pk, tk, solver_name;

  Real rhoi_old, omega;
  AllReduce<Real> global_res0;
  AllReduce<Real> global_res;
  AllReduce<Real> rhoi;
  AllReduce<Real> r0_dot_vk;
  AllReduce<Real> t_dot_s;
  AllReduce<Real> t_dot_t;
};

} // namespace solvers

} // namespace parthenon

#endif // SOLVERS_BICGSTAB_SOLVER_HPP_
