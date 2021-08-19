//========================================================================================
// (C) (or copyright) 2021. Triad National Security, LLC. All rights reserved.
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

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Local Includes
#include "bvals/cc/bvals_cc_in_one.hpp"
#include "interface/metadata.hpp"
#include "interface/update.hpp"
#include "mesh/meshblock_pack.hpp"
#include "mesh/refinement_cc_in_one.hpp"
#include "parthenon/driver.hpp"
#include "poisson_cg_driver.hpp"
#include "poisson_cg_package.hpp"
#include "refinement/refinement.hpp"


using namespace parthenon::driver::prelude;

namespace poisson_example {

parthenon::DriverStatus PoissonDriver::Execute() {
  pouts->MakeOutputs(pmesh, pinput);
  ConstructAndExecuteTaskLists<>(this);
  pouts->MakeOutputs(pmesh, pinput);
  return DriverStatus::complete;
}

TaskCollection PoissonDriver::MakeTaskCollection(BlockList_t &blocks) {
  using namespace parthenon::Update;
  using namespace parthenon::solvers;
  TaskCollection tc;
  TaskID none(0);

  for (int i = 0; i < blocks.size(); i++) {
    auto &pmb = blocks[i];
    auto &base = pmb->meshblock_data.Get();
  }

  int max_iters = pmesh->packages.Get("poisson_package")->Param<int>("max_iterations");
  int check_interval =
      pmesh->packages.Get("poisson_package")->Param<int>("check_interval");
  bool fail_flag =
      pmesh->packages.Get("poisson_package")->Param<bool>("fail_without_convergence");
  bool warn_flag =
      pmesh->packages.Get("poisson_package")->Param<bool>("warn_without_convergence");


  std::string pkg_name = "poisson_package";
  auto psn_pkg = pmesh->packages.Get("poisson_package");
  bool use_stencil = psn_pkg->Param<bool>("use_stencil");
  auto cgsol_stencil = (use_stencil
      ? psn_pkg->Param<std::shared_ptr<CG_Solver<Stencil<Real>>>>("cg_solver")
      : std::make_shared<CG_Solver<Stencil<Real>>>()); 
  auto cgsol_spmat = (!use_stencil
      ? psn_pkg->Param<std::shared_ptr<CG_Solver<SparseMatrixAccessor>>>("cg_solver")
      : std::make_shared<CG_Solver<SparseMatrixAccessor>>()); 
  std::vector<std::string> solver_vec_names;
  if (use_stencil) {
    solver_vec_names = cgsol_stencil->SolverState();
  } else {
    solver_vec_names = cgsol_spmat->SolverState();
  }
  
  const int num_partitions = pmesh->DefaultNumPartitions();
  TaskRegion &solver_region = tc.AddRegion(num_partitions);

  std::cout << "max_iters: " << max_iters
            << " checkinterval: " << check_interval
            << " fail flag: " << fail_flag
            << " warn_flag: " << warn_flag
            << " num_partitions: " << num_partitions<<std::endl;
  for (int i = 0; i < num_partitions; i++) {
    int reg_dep_id = 0; 
    // make/get a mesh_data container for the state
    auto &md = pmesh->mesh_data.GetOrAdd("base", i);

    TaskList &tl = solver_region[i];

    auto setrhs = tl.AddTask(none, poisson_package::SetRHS<MeshData<Real>>, md.get());
    auto mat_elem =
      tl.AddTask(none, poisson_package::SetMatrixElements<MeshData<Real>>, md.get());
    
    auto &solver = tl.AddIteration("poisson solver");
    solver.SetMaxIterations(max_iters);
    solver.SetCheckInterval(check_interval);
    solver.SetFailWithMaxIterations(fail_flag);
    solver.SetWarnWithMaxIterations(warn_flag);

    auto begin = setrhs | mat_elem;
    // create task list for solver.
    auto beta = (use_stencil 
              ? cgsol_stencil->createCGTaskList(begin, i, reg_dep_id, tc, tl, solver_region, solver, md, md)
              : cgsol_spmat->createCGTaskList(begin, i, reg_dep_id, tc, tl, solver_region, solver, md, md));

    auto print = none;
    if (i == 0) { // only print donce
      print = tl.AddTask(beta, poisson_package::PrintComplete);
    }
  }
  
  return tc;
}

} // namespace poisson_example
