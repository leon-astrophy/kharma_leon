// Seed a torus of some type with a magnetic field according to its density

#include "seed_B.hpp"

#include "phys.hpp"

// Internal representation of the field initialization preference for quick switch
// Avoids string comparsion in kernels
enum BSeedType{sane, ryan, r3s3, gaussian};

TaskStatus SeedBField(std::shared_ptr<Container<Real>>& rc, ParameterInput *pin)
{
    MeshBlock *pmb = rc->pmy_block;
    IndexDomain domain = IndexDomain::interior;
    int is = pmb->cellbounds.is(domain), ie = pmb->cellbounds.ie(domain);
    int js = pmb->cellbounds.js(domain), je = pmb->cellbounds.je(domain);
    int ks = pmb->cellbounds.ks(domain), ke = pmb->cellbounds.ke(domain);
    int n1 = pmb->cellbounds.ncellsi(IndexDomain::entire);
    int n2 = pmb->cellbounds.ncellsj(IndexDomain::entire);

    GRCoordinates G = pmb->coords;
    GridVars P = rc->Get("c.c.bulk.prims").data;

    Real rin = pin->GetOrAddReal("torus", "rin", 6.0);
    Real min_rho_q = pin->GetOrAddReal("b_field", "min_rho_q", 0.2);
    std::string b_field_type = pin->GetOrAddString("b_field", "type", "none");

    // Translate to an enum so we can avoid string comp inside,
    // as well as for good errors, many->one maps, etc.
    BSeedType b_field_flag = BSeedType::sane;
    if (b_field_type == "none") {
        return TaskStatus::complete;
    } else if (b_field_type == "sane") {
        b_field_flag = BSeedType::sane;
    } else if (b_field_type == "ryan") {
        b_field_flag = BSeedType::ryan;
    } else if (b_field_type == "r3s3") {
        b_field_flag = BSeedType::r3s3;
    } else if (b_field_type == "gaussian") {
        b_field_flag = BSeedType::gaussian;
    } else {
        throw std::invalid_argument("Magnetic field seed type not supported: " + b_field_type);
    }

    // Find the magnetic vector potential.  In X3 symmetry only A_phi is non-zero, so we keep track of that.
    ParArrayND<Real> A("A", n2, n1);
    // TODO figure out double vs Real here
    pmb->par_for("B_field_A", js, je+1, is, ie+1,
        KOKKOS_LAMBDA_2D {
            GReal Xembed[GR_DIM];
            G.coord_embed(0, j, i, Loci::center, Xembed);
            GReal r = Xembed[1], th = Xembed[2];

            // Find rho (later u?) at corners by averaging from adjacent centers
            Real rho_av = 0.25 * (P(prims::rho, NGHOST, j, i)     + P(prims::rho, NGHOST, j, i - 1) +
                                  P(prims::rho, NGHOST, j - 1, i) + P(prims::rho, NGHOST, j - 1, i - 1));

            Real q;
            switch (b_field_flag)
            {
            case BSeedType::sane:
                q = rho_av - min_rho_q;
                break;
            case BSeedType::ryan:
                // BR's smoothed poloidal in-torus
                q = pow(sin(th), 3) * pow(r / rin, 3) * exp(-r / 400) * rho_av - min_rho_q;
                break;
            case BSeedType::r3s3:
                // Just the r^3 sin^3 th term, proposed EHT standard MAD
                // TODO split r3 here and r3s3
                q = pow(r / rin, 3) * rho_av - min_rho_q;
                break;
            case BSeedType::gaussian:
                // Pure vertical threaded field of gaussian strength with FWHM 2*rin (i.e. HM@rin)
                // centered at BH center
                Real x = (r / rin) * sin(th);
                Real sigma = 2 / sqrt(2 * log(2));
                Real u = x / fabs(sigma);
                q = (1 / (sqrt(2 * M_PI) * fabs(sigma))) * exp(-u * u / 2);
                break;
            }

            A(j, i) = max(q, 0.);
        }
    );

    // Calculate B-field
    pmb->par_for("B_field_B", ks, ke, js, je, is, ie,
        KOKKOS_LAMBDA_3D {
            // Take a flux-ct step from the corner potentials
            P(prims::B1, k, j, i) = -(A(j, i) - A(j + 1, i) + A(j, i + 1) - A(j + 1, i + 1)) /
                                (2. * G.dx2v(j) * G.gdet(Loci::center, j, i));
            P(prims::B2, k, j, i) =  (A(j, i) + A(j + 1, i) - A(j, i + 1) - A(j + 1, i + 1)) /
                                (2. * G.dx1v(i) * G.gdet(Loci::center, j, i));
            P(prims::B3, k, j, i) = 0.;
        }
    );

    return TaskStatus::complete;
}

TaskStatus NormalizeBField(std::shared_ptr<Container<Real>>& rc, Real factor)
{
    MeshBlock *pmb = rc->pmy_block;
    IndexDomain domain = IndexDomain::interior;
    int is = pmb->cellbounds.is(domain), ie = pmb->cellbounds.ie(domain);
    int js = pmb->cellbounds.js(domain), je = pmb->cellbounds.je(domain);
    int ks = pmb->cellbounds.ks(domain), ke = pmb->cellbounds.ke(domain);
    GridVars P = rc->Get("c.c.bulk.prims").data;
    GridVars U = rc->Get("c.c.bulk.cons").data;
    GRCoordinates G = pmb->coords;

    // TODO *sigh*
    Real gamma = pmb->packages["GRMHD"]->Param<Real>("gamma");
    EOS* eos = CreateEOS(gamma);

    pmb->par_for("B_field_normalize", ks, ke, js, je, is, ie,
        KOKKOS_LAMBDA_3D {
            P(prims::B1, k, j, i) /= factor;
            P(prims::B2, k, j, i) /= factor;
            P(prims::B3, k, j, i) /= factor;

            FourVectors Dtmp;
            get_state(G, P, k, j, i, Loci::center, Dtmp);
            prim_to_flux(G, P, Dtmp, eos, k, j, i, Loci::center, 0, U);

        }
    );

    DelEOS(eos);
    return TaskStatus::complete;
}

Real GetLocalBetaMin(std::shared_ptr<Container<Real>>& rc)
{
    MeshBlock *pmb = rc->pmy_block;
    IndexDomain domain = IndexDomain::interior;
    int is = pmb->cellbounds.is(domain), ie = pmb->cellbounds.ie(domain);
    int js = pmb->cellbounds.js(domain), je = pmb->cellbounds.je(domain);
    int ks = pmb->cellbounds.ks(domain), ke = pmb->cellbounds.ke(domain);
    GRCoordinates G = pmb->coords;
    GridVars P = rc->Get("c.c.bulk.prims").data;

    // TODO *sigh*
    Real gamma = pmb->packages["GRMHD"]->Param<Real>("gamma");
    EOS* eos = CreateEOS(gamma);

    Real beta_min;
    Kokkos::Min<Real> min_reducer(beta_min);
    Kokkos::parallel_reduce("B_field_betamin",
        Kokkos::MDRangePolicy<Kokkos::Rank<3>>({ks, js, is}, {ke+1, je+1, ie+1}),
        KOKKOS_LAMBDA_3D_REDUCE {
            FourVectors Dtmp;
            get_state(G, P, k, j, i, Loci::center, Dtmp);
            double bsq_ij = bsq_calc(Dtmp);

            Real rho = P(prims::rho, k, j, i);
            Real u = P(prims::u, k, j, i);
            Real beta_ij = (eos->p(rho, u))/(0.5*(bsq_ij + TINY_NUMBER));

            if(beta_ij < local_result) local_result = beta_ij;
        }
    , min_reducer);

    DelEOS(eos);

    return beta_min;
}

// void SeedBHFlux(MeshBlock *pmb, GRCoordinates G, GridVars P, Real BHflux)
// {
//     MeshBlock *pmb = rc->pmy_block;
//     // This adds a central flux based on specifying some BHflux
//     // Initialize a net magnetic field inside the initial torus
//     // TODO do this only for BHflux > SMALL
//     pmb->par_for("BHflux_A", 0, N2, 0, N1,
//         KOKKOS_LAMBDA_2D {
//             Real Xembed[GR_DIM];
//             G.coord_embed(k, j, i, Loci::corner, Xembed);
//             Real r = Xembed[1], th = Xembed[2];

//             Real x = r * sin(th);
//             Real z = r * cos(th);
//             Real a_hyp = 20.;
//             Real b_hyp = 60.;
//             Real x_hyp = a_hyp * sqrt(1. + pow(z / b_hyp, 2));

//             Real q = (pow(x, 2) - pow(x_hyp, 2)) / pow(x_hyp, 2);
//             if (x < x_hyp) {
//                 A(j, i) = 10. * q;
//             } else {
//                 A(j, i) = 0.;
//             }
//         }
//     );

//     // Evaluate net flux
//     Real Phi_proc = 0.;
//     pmb->par_for("BHflux_B2net", 5, N1 - 1, 0, N2 - 1,
//         KOKKOS_LAMBDA_2D {
//             // TODO coord, some distance to M_PI/2
//             if (jglobal == N2TOT / 2)
//             {
//                 Real Xembed[GR_DIM];
//                 G.coord(k, j, i, Loci::center, Xembed);
//                 Real r = Xembed[1], th = Xembed[2];

//                 if (r < rin)
//                 {
//                     // Commented lines are unnecessary normalizations
//                     Real B2net = (A(j, i) + A(j + 1, i) - A(j, i + 1) - A(j + 1, i + 1));
//                     // / (2.*dx[1]*G.gdet(Loci::center, j, i));
//                     Phi_proc += fabs(B2net) * M_PI / N3CPU;
//                     // * 2.*dx[1]*G.gdet(Loci::center, j, i)
//                 }
//             }
//         }
//     );

//     // TODO ask if we're left bound in X1
//     if (global_start[0] == 0)
//     {
//         // TODO probably not globally safe
//         pmb->par_for("BHflux_B1net", 0, N2/2-1, 5+NG, 5+NG,
//             KOKKOS_LAMBDA_2D {
//                 Real B1net = -(A(j, i) - A(j + 1, i) + A(j, i + 1) - A(j + 1, i + 1));
//                 // /(2.*dx[2]*G.gdet(Loci::center, j, i));
//                 Phi_proc += fabs(B1net) * M_PI / N3CPU;
//                 // * 2.*dx[2]*G.gdet(Loci::center, j, i)
//             }
//         );
//     }
//     Real Phi = mpi_reduce(Phi_proc); // TODO this also needs to be max over meshes!!

//     Real norm = BHflux / (Phi + TINY_NUMBER);

//     pmb->par_for("BHflux_B", 0, n3-1, 0, n2-1, 0, n1-1,
//         KOKKOS_LAMBDA_3D {
//             // Flux-ct
//             P(prims::B1, k, j, i) += -norm * (A(j, i) - A(j + 1, i) + A(j, i + 1) - A(j + 1, i + 1)) /
//                                         (2. * pmb->dx2v(j) * G.gdet(Loci::center, j, i));
//             P(prims::B2, k, j, i) += norm * (A(j, i) + A(j + 1, i) - A(j, i + 1) - A(j + 1, i + 1)) /
//                                         (2. * pmb->dx1v(i) * G.gdet(Loci::center, j, i));
//         }
//     );
// }