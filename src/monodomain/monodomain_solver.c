//
// Created by sachetto on 03/10/17.
//

#include "monodomain_solver.h"
#include "../utils/file_utils.h"
#include "../utils/stop_watch.h"
#include "../libraries_common/common_data_structures.h"

#ifdef COMPILE_CUDA
#include "../gpu_utils/gpu_utils.h"
#endif

#ifdef COMPILE_OPENGL
#include "../draw/draw.h"
#endif

#include "../string/sds.h"
#include <assert.h>
#include <inttypes.h>

#include "../config/assembly_matrix_config.h"
#include "../config/domain_config.h"
#include "../config/purkinje_config.h"
#include "../config/stim_config.h"
#include "../config/linear_system_solver_config.h"

#include "../single_file_libraries/stb_ds.h"
#include "../config_helpers/config_helpers.h"

#include <unistd.h>

#include <stdio.h>
#include <float.h>

struct monodomain_solver *new_monodomain_solver() {

    struct monodomain_solver *result = (struct monodomain_solver *)malloc(sizeof(struct monodomain_solver));

    result->beta = 0.14;
    result->cm = 1.0;
    result->current_time = 0.0;
    result->current_count = 0;

    result->kappa_x = 0.0;
    result->kappa_y = 0.0;
    result->kappa_z = 0.0;
    
    return result;
}

int solve_monodomain(struct monodomain_solver *the_monodomain_solver, struct ode_solver *the_ode_solver,
                      struct grid *the_grid, struct user_options *configs) {

    assert(configs);

    assert(the_grid);
    assert(the_monodomain_solver);
    assert(the_ode_solver);

    print_to_stdout_and_file(LOG_LINE_SEPARATOR);

    long ode_total_time = 0, cg_total_time = 0, total_write_time = 0, total_mat_time = 0, total_ref_time = 0,
         total_deref_time = 0, cg_partial, total_config_time = 0;

    uint32_t total_cg_it = 0;

    struct stop_watch solver_time, ode_time, cg_time, part_solver, part_mat, write_time, ref_time, deref_time,
        config_time;

    init_stop_watch(&config_time);

    start_stop_watch(&config_time);

    ///////MAIN CONFIGURATION BEGIN//////////////////
    init_ode_solver_with_cell_model(the_ode_solver);
    struct string_voidp_hash_entry *stimuli_configs = configs->stim_configs;
    struct config *extra_data_config = configs->extra_data_config;
    struct config *domain_config = configs->domain_config;
    struct config *purkinje_config = configs->purkinje_config;
    struct config *assembly_matrix_config = configs->assembly_matrix_config;
    struct config *linear_system_solver_config = configs->linear_system_solver_config;
    struct config *save_mesh_config = configs->save_mesh_config;
    struct config *save_state_config = configs->save_state_config;
    struct config *restore_state_config = configs->restore_state_config;
    struct config *update_monodomain_config = configs->update_monodomain_config;

    bool has_extra_data = (extra_data_config != NULL);

    real_cpu last_stimulus_time = -1.0;
    bool has_any_periodic_stim = false;

    if(stimuli_configs) {
        // Init all stimuli
        STIM_CONFIG_HASH_FOR_INIT_FUNCTIONS(stimuli_configs);

        // Find last stimuli
        size_t s_size = shlen(stimuli_configs);
        real_cpu s_end;
        real_cpu stim_start = 0.0;
        real_cpu stim_duration = 0.0;
        real_cpu stim_period = 0;
        bool unnused;

        for(unsigned long i = 0; i < s_size; i++) {

            struct config *sconfig = (struct config*) stimuli_configs[i].value;

            GET_PARAMETER_NUMERIC_VALUE_OR_REPORT_ERROR(real_cpu, stim_start, sconfig->config_data, "start");
            GET_PARAMETER_NUMERIC_VALUE_OR_REPORT_ERROR(real_cpu, stim_duration, sconfig->config_data, "duration");
            GET_PARAMETER_NUMERIC_VALUE(real_cpu, stim_period, sconfig->config_data, "period", unnused);

            s_end = stim_start + stim_duration;

            has_any_periodic_stim |= (bool)(stim_period > 0.0);

            if(s_end > last_stimulus_time) {
                last_stimulus_time = s_end;
            }

        }
    }

    // Configure the functions and set the Purkinje mesh domain
    if (purkinje_config)
    {
        init_config_functions(purkinje_config, "shared_libs/libdefault_purkinje.so", "purkinje");
    }

    // Configure the functions and set the mesh domain
    if(domain_config) 
    {
        init_config_functions(domain_config, "./shared_libs/libdefault_domains.so", "domain");
    } 

    if( !purkinje_config && !domain_config ) {
        print_to_stderr_and_file_and_exit("Error configuring the domain! No Purkinje or tissue configuration was provided!\n");
    }


    if(assembly_matrix_config)
    {
        init_config_functions(assembly_matrix_config, "./shared_libs/libdefault_matrix_assembly.so", "assembly_matrix");
    } 
    else 
    {
        print_to_stderr_and_file_and_exit("No assembly matrix configuration provided! Exiting!\n");
    }

    if(linear_system_solver_config) 
    {
        init_config_functions(linear_system_solver_config, "./shared_libs/libdefault_linear_system_solver.so", "linear_system_solver");
    } 
    else 
    {
        print_to_stderr_and_file_and_exit("No linear solver configuration provided! Exiting!\n");
    }

    int print_rate = 0;
    GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(int, print_rate, save_mesh_config->config_data, "print_rate");

    char *out_dir_name = NULL;
    GET_PARAMETER_VALUE_CHAR_OR_USE_DEFAULT(out_dir_name, save_mesh_config->config_data, "output_dir");

    bool save_to_file = (save_mesh_config != NULL) && (print_rate > 0) && (out_dir_name);

    if(save_to_file) 
    {
        init_config_functions(save_mesh_config, "./shared_libs/libdefault_save_mesh.so", "save_result");
    } 
    else 
    {
        print_to_stdout_and_file("No configuration provided to save the results! The results will not be saved!\n");
    }

    bool save_checkpoint = (save_state_config != NULL);
    if(save_checkpoint) 
    {
        init_config_functions(save_state_config, "./shared_libs/libdefault_save_state.so", "save_state");
    } 
    else 
    {
        print_to_stdout_and_file(
            "No configuration provided to make simulation checkpoints! Chekpoints will not be created!\n");
    }

    bool restore_checkpoint = (restore_state_config != NULL);
    if(restore_state_config) 
    {
        init_config_functions(restore_state_config, "./shared_libs/libdefault_restore_state.so", "restore_state");
    }

    if(has_extra_data) 
    {
        init_config_functions(extra_data_config, "./shared_libs/libdefault_extra_data.so", "extra_data");
    }

    print_to_stdout_and_file(LOG_LINE_SEPARATOR);


    if(restore_checkpoint) 
    {
        // Here we only restore the monodomain_solver_state...
        ((restore_state_fn *)restore_state_config->main_function)(out_dir_name, restore_state_config, NULL,
                                            the_monodomain_solver, NULL);
    }

    if(update_monodomain_config) {
        init_config_functions(update_monodomain_config, "./shared_libs/libdefault_update_monodomain.so", "update_monodomain");
    }
    else {
        print_to_stderr_and_file_and_exit("No update monodomain configuration provided! Exiting!\n");
    }

    ///////MAIN CONFIGURATION END//////////////////
    int refine_each = the_monodomain_solver->refine_each;
    int derefine_each = the_monodomain_solver->derefine_each;

    bool redo_matrix;

    bool activity;

    #ifdef COMPILE_CUDA
    bool gpu = the_ode_solver->gpu;
    #endif

    int count = the_monodomain_solver->current_count;

    real_cpu refinement_bound = the_monodomain_solver->refinement_bound;
    real_cpu derefinement_bound = the_monodomain_solver->derefinement_bound;

    bool adaptive = the_grid->adaptive;
    real_cpu start_adpt_at = the_monodomain_solver->start_adapting_at;
    real_cpu dt_pde = the_monodomain_solver->dt;
    real_cpu finalT = the_monodomain_solver->final_time;
    real_cpu dt_ode = the_ode_solver->min_dt;

#ifdef COMPILE_CUDA
    if(gpu) {
        int device_count;
        int device = the_ode_solver->gpu_id;
        check_cuda_errors(cudaGetDeviceCount(&device_count));
        struct cudaDeviceProp prop;
        check_cuda_errors(cudaGetDeviceProperties(&prop, the_ode_solver->gpu_id));
        print_to_stdout_and_file("%d devices available, running on Device %d: %s\n", device_count, device, prop.name);
        check_cuda_errors(cudaSetDevice(device));
    }
#endif

    if(restore_checkpoint) 
    {
        // Here we only restore the grid...

        // TODO: Create a Purkinje restore function in the 'restore_library' and put here ...
        ((restore_state_fn*)restore_state_config->main_function)(out_dir_name, restore_state_config, the_grid, NULL, NULL);
    } 
    else 
    {
        int success;
        if (purkinje_config)
        {
            success = ((set_spatial_purkinje_fn*) purkinje_config->main_function)(purkinje_config,the_grid);
            if(!success) 
            {
                print_to_stderr_and_file_and_exit("Error configuring the Purkinje domain!\n");
            }
        }
            
        if (domain_config)
        {
            success = ((set_spatial_domain_fn*)domain_config->main_function)(domain_config, the_grid);

            if(configs->draw) {
                translate_mesh_to_origin(the_grid);
            }

            if(!success) {
                print_to_stderr_and_file_and_exit("Error configuring the tissue domain!\n");
            }
        }

        if (!purkinje_config && !domain_config)
        {
            print_to_stderr_and_file_and_exit("Error configuring the domain! No Purkinje or tissue configuration was provided!\n");
        }
    }

    real_cpu start_dx, start_dy, start_dz;
    real_cpu max_dx, max_dy, max_dz;

    start_dx = start_dy = start_dz = 100.0;
    max_dx = max_dy = max_dz = 100.0;

    if (purkinje_config)
    {
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dx, purkinje_config->config_data, "start_discretization");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dy, purkinje_config->config_data, "start_discretization");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dz, purkinje_config->config_data, "start_discretization");

        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dx, purkinje_config->config_data, "start_discretization");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dy, purkinje_config->config_data, "start_discretization");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dz, purkinje_config->config_data, "start_discretization");
    }

    if (domain_config)
    {
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dx, domain_config->config_data, "start_dx");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dy, domain_config->config_data, "start_dy");
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, start_dz, domain_config->config_data, "start_dz");

        if(!purkinje_config) {
            GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dx, domain_config->config_data, "maximum_dx");
            GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dy, domain_config->config_data, "maximum_dy");
            GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(real_cpu, max_dz, domain_config->config_data, "maximum_dz");
        }
    }

    order_grid_cells(the_grid);
    uint32_t original_num_cells = the_grid->num_active_cells;
    the_ode_solver->original_num_cells = original_num_cells;
    the_ode_solver->num_cells_to_solve = original_num_cells;

    save_old_cell_positions(the_grid);

    if(adaptive) 
    {
        update_cells_to_solve(the_grid, the_ode_solver);
    }
 
    if(has_extra_data)
        set_ode_extra_data(extra_data_config, the_grid, the_ode_solver);

    print_to_stdout_and_file("Setting ODE's initial conditions\n");
    set_ode_initial_conditions_for_all_volumes(the_ode_solver, configs->ode_extra_config);

    // We need to call this function after because of the pitch.... maybe we have to change the way
    // we pass this parameters to the cell model....
    if(restore_checkpoint) 
    {
        ((restore_state_fn*)restore_state_config->main_function)(out_dir_name, restore_state_config, NULL, NULL, the_ode_solver);
    }

    real_cpu initial_v = the_ode_solver->model_data.initial_v;

    total_config_time = stop_stop_watch(&config_time);

    print_solver_info(the_monodomain_solver, the_ode_solver, the_grid, configs);

    int ode_step = 1;

    if(dt_pde >= dt_ode) {
        ode_step = (int)(dt_pde / dt_ode);
        print_to_stdout_and_file("Solving EDO %d times before solving PDE\n", ode_step);
    } 
    else {
        print_to_stdout_and_file("WARNING: EDO time step is greater than PDE time step. Adjusting to EDO time "
                                 "step: %lf\n",
                                 dt_ode);
        dt_pde = dt_ode;
    }

    fflush(stdout);

    init_stop_watch(&solver_time);
    init_stop_watch(&ode_time);
    init_stop_watch(&cg_time);
    init_stop_watch(&part_solver);
    init_stop_watch(&part_mat);
    init_stop_watch(&write_time);
    init_stop_watch(&ref_time);
    init_stop_watch(&deref_time);

    start_stop_watch(&part_mat);

    if(!restore_checkpoint) {
        ((set_pde_initial_condition_fn*)assembly_matrix_config->init_function)(assembly_matrix_config, the_monodomain_solver, the_grid, initial_v);
    }

    ((assembly_matrix_fn*) assembly_matrix_config->main_function)(assembly_matrix_config, the_monodomain_solver, the_grid);

    total_mat_time = stop_stop_watch(&part_mat);
    start_stop_watch(&solver_time);

    int save_state_rate = 0;

    if(save_checkpoint) {
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(int, save_state_rate, save_state_config->config_data, "save_rate");
    }

    real_cpu vm_threshold = configs->vm_threshold;

    bool abort_on_no_activity = the_monodomain_solver->abort_on_no_activity;
    real_cpu solver_error;
    uint32_t solver_iterations = 0;

    real *spatial_stim_currents = NULL;

    if(stimuli_configs) {
        spatial_stim_currents = (real*)malloc(sizeof(real)*original_num_cells);
        set_spatial_stim(stimuli_configs, the_grid, spatial_stim_currents);
    }

    real_cpu cur_time = the_monodomain_solver->current_time;

    if(save_mesh_config != NULL) {
        GET_PARAMETER_NUMERIC_VALUE_OR_USE_DEFAULT(int, print_rate, save_mesh_config->config_data, "print_rate");
    }

    #ifdef COMPILE_OPENGL
    bool draw = configs->draw;
    if (draw) {
        draw_config.grid_info.grid_to_draw = the_grid;
        draw_config.simulating = true;
        draw_config.paused = !configs->start_visualization_unpaused;
    } else {
        draw_config.paused = false;
    }
    #endif

    print_to_stdout_and_file("Starting simulation\n");

    struct stop_watch iteration_time_watch;
    long iteration_time;

    init_stop_watch(&iteration_time_watch);

    CALL_INIT_LINEAR_SYSTEM(linear_system_solver_config, the_grid);

    // Main simulation loop start
    while(cur_time <= finalT)
    {
        start_stop_watch(&iteration_time_watch);

        #ifdef COMPILE_OPENGL
        if(draw) {
            omp_set_lock(&draw_config.sleep_lock);
            if (draw_config.restart) {
                draw_config.time = 0.0;

                CALL_END_LINEAR_SYSTEM(linear_system_solver_config);
                return RESTART_SIMULATION;
            }
            if (draw_config.exit)  {
                CALL_END_LINEAR_SYSTEM(linear_system_solver_config);
                return END_SIMULATION;
            }
        }
        #endif

        if (save_to_file && (count % print_rate == 0)) {
            start_stop_watch(&write_time);
            ((save_mesh_fn *)save_mesh_config->main_function)(save_mesh_config, the_grid, count, cur_time, finalT, dt_pde);
            total_write_time += stop_stop_watch(&write_time);
        }

        if (cur_time > 0.0) {
            activity = update_ode_state_vector_and_check_for_activity(vm_threshold, the_ode_solver, the_grid);

            if (abort_on_no_activity && cur_time > last_stimulus_time) {
                if (!activity) {
                    print_to_stdout_and_file("No activity, aborting simulation\n");
                    break;
                }
            }
        }

        start_stop_watch(&ode_time);

        // REACTION
        solve_all_volumes_odes(the_ode_solver, the_grid->num_active_cells, cur_time, ode_step, stimuli_configs, spatial_stim_currents, configs->ode_extra_config);
        ((update_monodomain_fn*)update_monodomain_config->main_function)(update_monodomain_config, original_num_cells, the_monodomain_solver, the_grid, the_ode_solver);

        ode_total_time += stop_stop_watch(&ode_time);

        start_stop_watch(&cg_time);

        #ifdef COMPILE_OPENGL
        if (draw) {
            omp_set_lock(&draw_config.draw_lock);
        }
        #endif

        // DIFUSION
        ((linear_system_solver_fn *)linear_system_solver_config->main_function)(linear_system_solver_config, the_grid, &solver_iterations, &solver_error);

        cg_partial = stop_stop_watch(&cg_time);

        cg_total_time += cg_partial;

        total_cg_it += solver_iterations;

        if (count % print_rate == 0) {
            print_to_stdout_and_file("t = %lf, Iterations = "
                                     "%" PRIu32 ", Error Norm = %e, Number of Cells:"
                                     "%" PRIu32 ", CG Iterations time: %ld us",
                                     cur_time, solver_iterations, solver_error, the_grid->num_active_cells,
                                     cg_partial);
        }

        if (adaptive) {
            redo_matrix = false;
            if (cur_time >= start_adpt_at) {
                if (count % refine_each == 0) {

                    start_stop_watch(&ref_time);
                    redo_matrix = refine_grid_with_bound(the_grid, refinement_bound, start_dx, start_dy, start_dz);
                    total_ref_time += stop_stop_watch(&ref_time);
                }

                if (count % derefine_each == 0) {
                    start_stop_watch(&deref_time);
                    redo_matrix |= derefine_grid_with_bound(the_grid, derefinement_bound, max_dx, max_dy, max_dz);
                    total_deref_time += stop_stop_watch(&deref_time);
                }
            }
            if (redo_matrix) {
                order_grid_cells(the_grid);

                if (stimuli_configs) {
                    if (cur_time <= last_stimulus_time || has_any_periodic_stim) {
                        free(spatial_stim_currents);
                        spatial_stim_currents = (real*)malloc(sizeof(real)*the_grid->num_active_cells);
                        set_spatial_stim(stimuli_configs, the_grid, spatial_stim_currents);
                    }
                }
                if (has_extra_data) {
                    set_ode_extra_data(extra_data_config, the_grid, the_ode_solver);
                }

                update_cells_to_solve(the_grid, the_ode_solver);

                if (arrlen(the_grid->refined_this_step) > 0) {
                    update_state_vectors_after_refinement(the_ode_solver, the_grid->refined_this_step);
                }

                start_stop_watch(&part_mat);
                ((assembly_matrix_fn *)assembly_matrix_config->main_function)(assembly_matrix_config, the_monodomain_solver, the_grid);

                total_mat_time += stop_stop_watch(&part_mat);
            }

        }

        #ifdef COMPILE_OPENGL
        if (configs->draw) {
            omp_unset_lock(&draw_config.draw_lock);
            draw_config.time = cur_time;
        }
        #endif
        count++;
        cur_time += dt_pde;

        if (save_checkpoint) {
            if (count != 0 && (count % save_state_rate == 0)) {
                the_monodomain_solver->current_count = count;
                the_monodomain_solver->current_time = cur_time;
                printf("Saving state with time = %lf, and count = %d\n", the_monodomain_solver->current_time,
                       the_monodomain_solver->current_count);
                ((save_state_fn *)save_state_config->main_function)(out_dir_name, save_state_config, the_grid, the_monodomain_solver, the_ode_solver);
            }
        }


        iteration_time = stop_stop_watch(&iteration_time_watch);

        if ( (count - 1) % print_rate == 0) {
            print_to_stdout_and_file(", Total Iteration time: %ld us\n", iteration_time);
        }
    }

    long res_time = stop_stop_watch(&solver_time);
    print_to_stdout_and_file("Resolution Time: %ld μs\n", res_time);
    print_to_stdout_and_file("ODE Total Time: %ld μs\n", ode_total_time);
    print_to_stdout_and_file("CG Total Time: %ld μs\n", cg_total_time);
    print_to_stdout_and_file("Mat time: %ld μs\n", total_mat_time);
    print_to_stdout_and_file("Refine time: %ld μs\n", total_ref_time);
    print_to_stdout_and_file("Derefine time: %ld μs\n", total_deref_time);
    print_to_stdout_and_file("Write time: %ld μs\n", total_write_time);
    print_to_stdout_and_file("Initial configuration time: %ld μs\n", total_config_time);
    print_to_stdout_and_file("CG Total Iterations: %u\n", total_cg_it);

#ifdef COMPILE_OPENGL
   draw_config.solver_time = res_time;
   draw_config.ode_total_time = ode_total_time;
   draw_config.cg_total_time = cg_total_time;
   draw_config.total_mat_time = total_mat_time;
   draw_config.total_ref_time = total_ref_time;
   draw_config.total_deref_time = total_deref_time;
   draw_config.total_write_time = total_write_time;
   draw_config.total_config_time = total_config_time;
   draw_config.total_cg_it  = total_cg_it;
   draw_config.simulating = false;
#endif

    CALL_END_LINEAR_SYSTEM(linear_system_solver_config);
    return SIMULATION_FINISHED;

}

void set_spatial_stim(struct string_voidp_hash_entry *stim_configs, struct grid *the_grid, real *spatial_stim_currents) {

    struct config *tmp = NULL;
    size_t n = shlen(stim_configs);

    for(size_t i = 0; i < n; i++) {
        tmp = (struct config *)stim_configs[i].value;
        ((set_spatial_stim_fn*)tmp->main_function)(tmp, the_grid, spatial_stim_currents);
    }
}

void set_ode_extra_data(struct config *config, struct grid *the_grid, struct ode_solver *the_ode_solver) {

    free(the_ode_solver->ode_extra_data);
    the_ode_solver->ode_extra_data =
            ((set_extra_data_fn*)config->main_function)(the_grid, config, &(the_ode_solver->extra_data_size));
}

bool update_ode_state_vector_and_check_for_activity(real_cpu vm_threshold, struct ode_solver *the_ode_solver,
                                                    struct grid *the_grid) {

    uint32_t n_active = the_grid->num_active_cells;
    struct cell_node **ac = the_grid->active_cells;

    int n_odes = the_ode_solver->model_data.number_of_ode_equations;

    real *sv = the_ode_solver->sv;

    bool act = false;

    if(the_ode_solver->gpu) {
    #ifdef COMPILE_CUDA
        uint32_t max_number_of_cells = the_ode_solver->original_num_cells;
        real *vms;
        size_t mem_size = max_number_of_cells * sizeof(real);

        vms = (real *)malloc(mem_size);

        if(the_grid->adaptive)
            check_cuda_errors(cudaMemcpy(vms, sv, mem_size, cudaMemcpyDeviceToHost));

        #pragma omp parallel for
        for(uint32_t i = 0; i < n_active; i++) {
            vms[ac[i]->sv_position] = (real)ac[i]->v;

            if(ac[i]->v > vm_threshold) {
                act = true;
            }
        }

        check_cuda_errors(cudaMemcpy(sv, vms, mem_size, cudaMemcpyHostToDevice));
        free(vms);
    #endif
    } else {
        #pragma omp parallel for
        for(uint32_t i = 0; i < n_active; i++) {
            sv[ac[i]->sv_position * n_odes] = (real)ac[i]->v;

            if(ac[i]->v > vm_threshold) {
                act = true;
            }
        }
    }

    return act;
}

void save_old_cell_positions(struct grid *the_grid) {

    uint32_t n_active = the_grid->num_active_cells;
    struct cell_node **ac = the_grid->active_cells;

    int i;

    #pragma omp parallel for
    for(i = 0; i < n_active; i++) {
        ac[i]->sv_position = ac[i]->grid_position;
    }
}

void update_cells_to_solve(struct grid *the_grid, struct ode_solver *solver) {

    uint32_t n_active = the_grid->num_active_cells;
    struct cell_node **ac = the_grid->active_cells;

    if(solver->cells_to_solve) {
        free(solver->cells_to_solve);
    }

    solver->num_cells_to_solve = n_active;
    solver->cells_to_solve = (uint32_t *)malloc(n_active * sizeof(uint32_t));
    uint32_t *cts = solver->cells_to_solve;
    int i;

    #pragma omp parallel for
    for(i = 0; i < n_active; i++) {
        cts[i] = ac[i]->sv_position;
    }
}


void print_solver_info(struct monodomain_solver *the_monodomain_solver, struct ode_solver *the_ode_solver,
                       struct grid *the_grid, struct user_options *options) {

    print_to_stdout_and_file(LOG_LINE_SEPARATOR);

    print_to_stdout_and_file("System parameters: \n");
    #if defined(_OPENMP)
    print_to_stdout_and_file("Using OpenMP with %d threads\n", omp_get_max_threads());
    #endif
    if(the_ode_solver->gpu) {
        print_to_stdout_and_file("Using GPU to solve ODEs\n");
    }

    print_to_stdout_and_file("Using %s as model lib\n", the_ode_solver->model_data.model_library_path);

    print_to_stdout_and_file("Initial V: %lf\n", the_ode_solver->model_data.initial_v);
    print_to_stdout_and_file("Number of ODEs in cell model: %d\n", the_ode_solver->model_data.number_of_ode_equations);

    print_to_stdout_and_file("Beta = %.10lf, Cm = %.10lf\n", the_monodomain_solver->beta, the_monodomain_solver->cm);

    print_to_stdout_and_file("Initial N. of Elements = "
                             "%" PRIu32 "\n",
                             the_grid->num_active_cells);
    print_to_stdout_and_file("PDE time step = %lf\n", the_monodomain_solver->dt);
    print_to_stdout_and_file("ODE min time step = %lf\n", the_ode_solver->min_dt);
    print_to_stdout_and_file("Simulation Final Time = %lf\n", the_monodomain_solver->final_time);

    print_to_stdout_and_file(LOG_LINE_SEPARATOR);

    if(options->ode_extra_config) {

        if (shlen(options->ode_extra_config) == 1) {
            print_to_stdout_and_file("Extra ODE Solver parameter:\n");
        } else if (shlen(options->ode_extra_config) > 1) {
            print_to_stdout_and_file("Extra ODE Solver parameters:\n");
        }

        STRING_HASH_PRINT_KEY_VALUE_LOG(options->ode_extra_config);
    }

    print_to_stdout_and_file(LOG_LINE_SEPARATOR);

    if(the_grid->adaptive)
    {
        print_to_stdout_and_file("Using adaptativity\n");
        print_to_stdout_and_file("Refinement Bound = %lf\n", the_monodomain_solver->refinement_bound);
        print_to_stdout_and_file("Derefinement Bound = %lf\n", the_monodomain_solver->derefinement_bound);
        print_to_stdout_and_file("Refining each %d time steps\n", the_monodomain_solver->refine_each);
        print_to_stdout_and_file("Derefining each %d time steps\n", the_monodomain_solver->derefine_each);

        char *max_dx, *max_dy, *max_dz;

        max_dx = shget(options->domain_config->config_data, "maximum_dx");
        max_dy = shget(options->domain_config->config_data, "maximum_dy");
        max_dz = shget(options->domain_config->config_data, "maximum_dz");

        print_to_stdout_and_file("Domain maximum Space Discretization: dx %s um, dy %s um, dz %s um\n", max_dx, max_dy, max_dz);


        print_to_stdout_and_file("The adaptivity will start in time: %lf ms\n",
                                 the_monodomain_solver->start_adapting_at);
    }

    if(options->linear_system_solver_config) {
        print_linear_system_solver_config_values(options->linear_system_solver_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }

    if(options->save_mesh_config) {
        print_save_mesh_config_values(options->save_mesh_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }

    if(options->stim_configs) 
    {

        size_t num_stims = shlen(options->stim_configs);

        if(num_stims == 1)
            print_to_stdout_and_file("Stimulus configuration:\n");
        else
            print_to_stdout_and_file("Stimuli configuration:\n");

        for(int i = 0; i < num_stims; i++) {

            struct string_voidp_hash_entry e = options->stim_configs[i];
            print_to_stdout_and_file("Stimulus name: %s\n", e.key);
            print_stim_config_values((struct config*) e.value);
            print_to_stdout_and_file(LOG_LINE_SEPARATOR);

        }
    }

    if (options->domain_config)
    {
        print_domain_config_values(options->domain_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }

    if (options->purkinje_config)
    {
        print_purkinje_config_values(options->purkinje_config);
        print_to_stdout_and_file (LOG_LINE_SEPARATOR);
    }

    if(options->extra_data_config) {
        print_extra_data_config_values(options->extra_data_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }

    if(options->update_monodomain_config) {
        print_update_monodomain_config_values(options->update_monodomain_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }

    if(options->assembly_matrix_config) {
        print_assembly_matrix_config_values(options->assembly_matrix_config);
        print_to_stdout_and_file(LOG_LINE_SEPARATOR);
    }
}

void configure_monodomain_solver_from_options(struct monodomain_solver *the_monodomain_solver,
                                              struct user_options *options) {

    assert(the_monodomain_solver);
    assert(options);

    the_monodomain_solver->num_threads = options->num_threads;
    the_monodomain_solver->final_time = options->final_time;

    the_monodomain_solver->refine_each = options->refine_each;
    the_monodomain_solver->derefine_each = options->derefine_each;
    the_monodomain_solver->refinement_bound = options->ref_bound;
    the_monodomain_solver->derefinement_bound = options->deref_bound;

    the_monodomain_solver->abort_on_no_activity = options->abort_no_activity;

    the_monodomain_solver->dt = options->dt_pde;

    the_monodomain_solver->beta = options->beta;
    the_monodomain_solver->cm = options->cm;
    the_monodomain_solver->start_adapting_at = options->start_adapting_at;
}

