#ifndef GITRM_PUSH_HPP
#define GITRM_PUSH_HPP

#include "GitrmMesh.hpp"
#include "GitrmParticles.hpp" 

#include <SellCSigma.h>
#include <SCS_Macros.h>
#include <Kokkos_Core.hpp>  //direct use


#include <iostream>
#include <cmath>
#include <utility>

#include "Omega_h_adj.hpp"
#include "Omega_h_array_ops.hpp"
#include "Omega_h_scalar.hpp" //divide
#include "Omega_h_fail.hpp"

#include "pumipic_utils.hpp"
#include "pumipic_constants.hpp"



inline void gitrm_getE(particle_structs::SellCSigma<Particle>* scs, const o::Mesh &mesh) {

  //TODO check
  const auto angles = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::FACE, "angleBdryBfield")));
  const auto potentials = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::FACE, "potential")));
  const auto debyeLengths = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::FACE, "DebyeLength")));
  const auto larmorRadii = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::FACE, "LarmorRadius")));
  const auto childLangmuirDists = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::FACE, "ChildLangmuirDist")));


  scs->transferToDevice();
  kkFp3View ptclPos_d("ptclPos_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(ptclPos_d, scs->getSCS<PCL_POS>());
  
  kkFp3View closestPoint_d("closestPoint_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(closestPoint_d, scs->getSCS<PCL_BDRY_CLOSEPT>()); 

  kkLidView faceId_d("faceId_d", scs->offsets[scs->num_slices]);
  hostToDeviceLid(faceId_d, scs->getSCS<PCL_BDRY_FACEID>());
  
  kkFp3View efield_d("efield_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(efield_d, scs->getSCS<PCL_EFIELD_PREV>());

  //TODO hostToDevice ...
  auto run = SCS_LAMBDA(const int &elem, const int &pid,
                                const int &mask) { 
    o::LO verbose = (elem%500==0)?3:0;

    auto faceId = faceId_d[pid];
    if(faceId < 0){
      //TODO check
      efield_d(pid, 0) = 0;
      efield_d(pid, 1) = 0;
      efield_d(pid, 2) = 0;
      return;
    }

    o::Real angle = angles[faceId];
    o::Real pot = potentials[faceId];
    o::Real debyeLength = debyeLengths[faceId];
    o::Real larmorRadius = larmorRadii[faceId];
    o::Real childLangmuirDist = childLangmuirDists[faceId];

    // TODO utility function 
    o::Vector<3> pos{ptclPos_d(pid,0), ptclPos_d(pid,1), ptclPos_d(pid,2)};
    o::Vector<3> closest{closestPoint_d(pid,0), closestPoint_d(pid,1), 
      closestPoint_d(pid,2)};
    o::Vector<3> distVector = pos - closest; 
    o::Vector<3> directionUnitVector = o::normalize(distVector);
    o::Real md = p::osh_mag(distVector);
    o::Real Emag = 0;

    if(BIASED_SURFACE) {
      Emag = pot/(2.0*childLangmuirDist)*
              exp(-md/(2.0*childLangmuirDist));
    }
    else { 
      o::Real fd = 0.98992 + 5.1220E-03 * angle - 7.0040E-04 * pow(angle,2.0) +
                   3.3591E-05 * pow(angle,3.0) - 8.2917E-07 * pow(angle,4.0) +
                   9.5856E-09 * pow(angle,5.0) - 4.2682E-11 * pow(angle,6.0);
      
      Emag = pot*(fd/(2.0 * debyeLength)* exp(-md/(2.0 * debyeLength))+ 
              (1.0 - fd)/(larmorRadius)* exp(-md/larmorRadius));
    }


    if(p::almost_equal(md, 0.0) || p::almost_equal(larmorRadius, 0.0)) {
      Emag = 0.0;
      directionUnitVector = {0, 0, 0}; //TODO confirm
    }
    auto exd = Emag*directionUnitVector;
    efield_d(pid, 0) = exd[0];
    efield_d(pid, 1) = exd[1];
    efield_d(pid, 2) = exd[2];

    if(verbose >2)
     printf("efield %.5f %.5f %.5f \n", efield_d(pid, 0), efield_d(pid, 1), efield_d(pid, 2));

  };
  scs->parallel_for(run);

  deviceToHostFp(efield_d, scs->getSCS<PCL_EFIELD_PREV>());
}


//TODO
OMEGA_H_INLINE void interpolateBField(const o::Reals &BField, const o::Vector<3> &posPrev,
  o::Vector<3> &bField) {
  srand (time(NULL));
  bField[0] = (double)(std::rand())/RAND_MAX;
  bField[1] = (double)(std::rand())/RAND_MAX;
  bField[2] = (double)(std::rand())/RAND_MAX;
}

    
inline void gitrm_borisMove(particle_structs::SellCSigma<Particle>* scs, 
  const o::Mesh &mesh, const o::Real dtime) {

  const auto BField = o::Reals(o::deep_copy(
      mesh.get_array<o::Real>(o::VERT, "BField")));

  scs->transferToDevice();
  kkFp3View efield_d("efield_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(efield_d, scs->getSCS<PCL_EFIELD_PREV>());
  kkFp3View vel_d("vel_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(vel_d, scs->getSCS<PCL_VEL>());
  kkFp3View ptclPrevPos_d("ptclPrevPos_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(ptclPrevPos_d, scs->getSCS<PCL_POS_PREV>());
  kkFp3View ptclPos_d("ptclPos_d", scs->offsets[scs->num_slices]);
  hostToDeviceFp(ptclPos_d, scs->getSCS<PCL_POS>());

  auto boris = SCS_LAMBDA(const int &elem, const int &pid, const int &mask) {
    o::LO verbose = (elem%500==0)?3:0;

    //TODO check
    Omega_h::Vector<3> vel{vel_d(pid,0), vel_d(pid,1), vel_d(pid,2)};  //at current_pos
    Omega_h::Vector<3> eField{efield_d(pid,0), efield_d(pid,1),efield_d(pid,2)}; //previous_pos
    Omega_h::Vector<3> posPrev{ptclPrevPos_d(pid,0), ptclPrevPos_d(pid,1), ptclPrevPos_d(pid,2)};
    Omega_h::Vector<3> bField;
    interpolateBField(BField, posPrev, bField); //previous_pos

    o::Real charge = 1; //TODO get using speciesID using enum
    o::Real amu = 184.0; //TODO //impurity_amu = 184.0

    OMEGA_H_CHECK(amu >0 && dtime>0); //TODO dtime
    Omega_h::Real bFieldMag = p::osh_mag(bField);
    Omega_h::Real qPrime = charge*1.60217662e-19/(amu*1.6737236e-27) *dtime*0.5;
    Omega_h::Real coeff = 2.0*qPrime/(1.0+(qPrime*bFieldMag)*(qPrime*bFieldMag));

      //v_minus = v + q_prime*E;
    Omega_h::Vector<3> qpE = qPrime*eField;
    Omega_h::Vector<3> vMinus = vel - qpE;

    //v_prime = v_minus + q_prime*(v_minus x B)
    Omega_h::Vector<3> vmxB = Omega_h::cross(vMinus,bField);
    Omega_h::Vector<3> qpVmxB = qPrime*vmxB;
    Omega_h::Vector<3> vPrime = vMinus + qpVmxB;

    //v = v_minus + coeff*(v_prime x B)
    Omega_h::Vector<3> vpxB = Omega_h::cross(vPrime, bField);
    Omega_h::Vector<3> cVpxB = coeff*vpxB;
    vel = vMinus + cVpxB;

    //v = v + q_prime*E
    vel = vel + qpE;

    //write
    Omega_h::Vector<3> pre = {ptclPrevPos_d(pid, 0), ptclPrevPos_d(pid, 1), 
                              ptclPrevPos_d(pid, 2)}; //prev pos
    ptclPrevPos_d(pid, 0) = ptclPos_d(pid, 0);
    ptclPrevPos_d(pid, 1) = ptclPos_d(pid, 1);
    ptclPrevPos_d(pid, 2) = ptclPos_d(pid, 2);

    // Next position and velocity
    ptclPos_d(pid, 0) = pre[0] + vel[0] * dtime;
    ptclPos_d(pid, 1) = pre[1] + vel[1] * dtime;
    ptclPos_d(pid, 2) = pre[2] + vel[2] * dtime;
    vel_d(pid, 0) = vel[0];
    vel_d(pid, 1) = vel[1];
    vel_d(pid, 2) = vel[2];
    
    if(verbose >2){
      printf("prev_pos: %.3f %.3f %.3f \n", ptclPrevPos_d(pid, 0), ptclPrevPos_d(pid, 1), 
        ptclPrevPos_d(pid, 2));
      printf("pre: %.3f %.3f %.3f \n", pre[0], pre[1], pre[2]);
      printf("pos: %.3f %.3f %.3f \n", ptclPos_d(pid, 0), ptclPos_d(pid, 1), ptclPos_d(pid, 2));
      printf("vel: %.3f %.3f %.3f \n", vel_d(pid, 0), vel_d(pid, 1), vel_d(pid, 2));
    }
  };

  scs->parallel_for(boris);
  deviceToHostFp(ptclPos_d, scs->getSCS<PCL_POS>());
  deviceToHostFp(ptclPrevPos_d, scs->getSCS<PCL_POS_PREV>());
  deviceToHostFp(vel_d, scs->getSCS<PCL_VEL>());
}



#endif //define