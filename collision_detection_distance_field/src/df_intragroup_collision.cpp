/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Acorn Pooley */

#include <moveit/collision_detection_distance_field/collision_robot_distance_field.h>
#include "collision_robot_distance_field_inline.h"
#include "aabb.h"
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/body_operations.h>
#include <console_bridge/console.h>
#include <cassert>

#include <moveit/distance_field/distance_field_common.h>
#include <moveit/distance_field/propagation_distance_field.h>

namespace
{
bool verifyMatrixIsIsometric(const Eigen::Affine3d& m, const std::string& name)
{
  Eigen::Quaterniond qrot(m.linear());
  Eigen::Matrix3d rot(qrot);
  if (!m.linear().isApprox(rot, 0.001))
  {
    logError("Link %s GlobalCollisionBodyTransform is not pure rotation.  DF will not work.",name.c_str());
    logError("    GlobalCollisionBodyTransform: %12.6f %12.6f %12.6f %12.6f rot: %12.6f %12.6f %12.6f %12.6f",
      m(0,0),
      m(0,1),
      m(0,2),
      m(0,3),
      rot(0,0),
      rot(0,1),
      rot(0,2),
      m(0,3));
    logError("    GlobalCollisionBodyTransform: %12.6f %12.6f %12.6f %12.6f rot: %12.6f %12.6f %12.6f %12.6f",
      m(1,0),
      m(1,1),
      m(1,2),
      m(1,3),
      rot(1,0),
      rot(1,1),
      rot(1,2),
      m(1,3));
    logError("    GlobalCollisionBodyTransform: %12.6f %12.6f %12.6f %12.6f rot: %12.6f %12.6f %12.6f %12.6f",
      m(2,0),
      m(2,1),
      m(2,2),
      m(2,3),
      rot(2,0),
      rot(2,1),
      rot(2,2),
      m(2,3));
    logError("    GlobalCollisionBodyTransform: %12.6f %12.6f %12.6f %12.6f rot: %12.6f %12.6f %12.6f %12.6f",
      m(3,0),
      m(3,1),
      m(3,2),
      m(3,3),
      m(3,0),
      m(3,1),
      m(3,2),
      m(3,3));
    return false;
  }
  return true;
}
}

void collision_detection::CollisionRobotDistanceField::initLinkDF()
{
  max_df_distance_ = max_bounding_sphere_radius_ + MAX_DISTANCE_;

  links_.resize(link_order_.size());
  all_links_.resize(link_order_.size() + 1);
  all_links_[link_order_.size()] = NULL; 

  // Just used to assert that the collision transform is isometric (rotation
  // and translation only).
  robot_state::RobotState state(kmodel_);
  state.setToDefaultValues();

  std::vector<bool> valid_links;
  valid_links.resize(link_order_.size(), false);

  // generate a distance field for each link with geometry
  for ( int i = 0 ; i < link_order_.size() ; ++i )
  {
    const robot_model::LinkModel* lm = linkIndexToLinkModel(i);
    logInform("Prepare link %s",lm->getName().c_str());
    DFLink& link = links_[i];
    all_links_[i] = &links_[i];

    link.name_ = &lm->getName();
    link.index_in_link_order_ = i;
    link.index_in_model_ = link_order_[i];

    // initialize collision matrix
    link.acm_bits_.reset(link_order_.size(), false);
    link.acm_bits_.setBit(i);

    // set padding
    link.padding_ = 0.0;
    std::map<std::string, double>::const_iterator padding_it = link_padding_.find(*link.name_);
    if (padding_it != link_padding_.end())
      link.padding_ = padding_it->second;

    // set scale
    link.scale_ = 1.0;
    std::map<std::string, double>::const_iterator scale_it = link_scale_.find(*link.name_);
    if (scale_it != link_scale_.end())
      link.scale_ = scale_it->second;

    // find list of spheres for this link
    link.sphere_idx_begin_ = 0;
    link.sphere_idx_end_ = 0;
    std::vector<SphereIndex>::const_iterator spidx = sphere_idx_to_link_index_.begin();
    std::vector<SphereIndex>::const_iterator spidx_end = sphere_idx_to_link_index_.end();
    for ( ; spidx != spidx_end ; ++spidx )
    {
      if (*spidx == i)
      {
        link.sphere_idx_begin_ = spidx - sphere_idx_to_link_index_.begin();
        for ( ; spidx != spidx_end && *spidx == i; ++spidx )
        {
          link.sphere_idx_end_ = spidx - sphere_idx_to_link_index_.begin() + 1;
        }
        break;
      }
    }

    // Verify that there is no scaling or shear in the transforms.
    if (!verifyMatrixIsIsometric(linkIndexToLinkState(i, &state)->getGlobalCollisionBodyTransform(), lm->getName()))
    {
      logError("initLinkDF: Link %d '%s' has bad transform.", i, lm->getName().c_str());
      continue;
    }

    logInform("Generate DISTANCE FIELD for link %s",lm->getName().c_str());

    const shapes::ShapeConstPtr& shape = lm->getShape();
    if (!shape)
    {
      logError("initLinkDF: Link %d '%s' has no shape.", i, lm->getName().c_str());
      continue;
    }

    logInform("    create body");
    bodies::Body *body = bodies::createBodyFromShape(shape.get());
    if (!body)
    {
      logError("initLinkDF: could not create body from shape for Link %d '%s' .", i, lm->getName().c_str());
      continue;
    }

    body->setScale(link.scale_);

    #define DEBUG_SAVE_STATIC_DF_POINTS true
    link.df_.initialize(*body, SELF_COLLISION_RESOLUTION_, max_df_distance_, DEBUG_SAVE_STATIC_DF_POINTS);

    delete body;

    valid_links[i] = true;
  }


  // check default collision matrix from srdf
  std::vector<srdf::Model::DisabledCollision>::const_iterator it = kmodel_->getSRDF()->getDisabledCollisionPairs().begin();
  std::vector<srdf::Model::DisabledCollision>::const_iterator it_end = kmodel_->getSRDF()->getDisabledCollisionPairs().end();
  for ( ; it != it_end ; ++it)
  {
    int link_idx_a = linkNameToIndex(it->link1_);
    int link_idx_b = linkNameToIndex(it->link2_);
    if (link_idx_a < 0 || link_idx_b < 0)
      continue;

    DFLink& link_a = links_[link_idx_a];
    DFLink& link_b = links_[link_idx_b];

    link_a.acm_bits_.setBit(link_idx_b);
    link_b.acm_bits_.setBit(link_idx_a);
  }
}

void collision_detection::CollisionRobotDistanceField::checkSelfCollisionUsingIntraDFLoop(
    WorkArea& work,
    const DFLink * const *link_list) const
{
  DFContact dummy_dfcontact;
  DFContact *dfcontact = &dummy_dfcontact;
  

  for (const DFLink * const *p_link_a = link_list ; p_link_a[1] ; ++p_link_a)
  {
    const DFLink *link_a = *p_link_a;
    robot_state::LinkState *lsa = work.state1_->getLinkStateVector()[link_a->index_in_model_];
    
    Eigen::Affine3d pf_to_linka = lsa->getGlobalCollisionBodyTransform().inverse(Eigen::Isometry);

    for (const DFLink * const *p_link_b = p_link_a + 1 ; *p_link_b ; ++p_link_b)
    {
      const DFLink *link_b = *p_link_b;

      if (never_check_link_pair(link_a, link_b))
        continue;

      double padding = link_a->df_.getResolution() + link_a->padding_ + link_b->padding_;

      robot_state::LinkState *lsb = work.state1_->getLinkStateVector()[link_b->index_in_model_];
      Eigen::Affine3d linkb_to_linka = pf_to_linka * lsb->getGlobalCollisionBodyTransform();

      Eigen::Vector3d bsphere_center = linkb_to_linka * bounding_sphere_centers_[link_b->index_in_link_order_];

      const DistPosEntry& bs_entry = link_a->df_(bsphere_center);
      double dist = bs_entry.distance_ - bounding_sphere_radii_[link_b->index_in_link_order_] - padding;

      if (dist > 0)
      {
        setCloseDistance(work, dist);
        continue;
      }
      

      const AllowedCollisionMatrix *acm = work.acm_;
      collision_detection::DecideContactFn acm_condition;
      std::vector<Contact>* pair_contacts = NULL;

      for (int i = link_b->sphere_idx_begin_ ; i != link_b->sphere_idx_end_ ; ++i)
      {
        Eigen::Vector3d center = linkb_to_linka * sphere_centers_[i];

        const DistPosEntry& entry = link_a->df_(center);
        dist = entry.distance_ - sphere_radii_[i] - padding;

        if (dist > 0)
        {
          setCloseDistance(work, dist);
          continue;
        }

        if (acm)
        {
          AllowedCollision::Type allowed_collision;
          if (acm->getAllowedCollision(*link_a->name_, *link_b->name_, allowed_collision))
          {
            if (allowed_collision == collision_detection::AllowedCollision::NEVER)
            {
              // nothing to do here.  This is the common case so it is first.
            }
            else if (allowed_collision == collision_detection::AllowedCollision::ALWAYS)
            {
              goto next_link;  // collisions allowed between this link pair, so break out to next link.
            }
            else if (allowed_collision == collision_detection::AllowedCollision::CONDITIONAL)
            {
              if (!acm->getAllowedCollision(*link_a->name_, *link_b->name_, acm_condition))
              {
                logWarn("collision type conditional but no function for links '%s' <--> '%s'",
                    link_a->name_->c_str(),
                    link_b->name_->c_str());
                acm_condition = 0;
              }
            }
          }
          acm = NULL; // do not check again for this link pair
        }

        if (acm_condition || work.req_->contacts)
        {
          Contact contact;
          Eigen::Vector3d pos;
          link_a->df_.getCellPosition(entry.cell_id_, pos);
          contact.pos = lsa->getGlobalCollisionBodyTransform() * pos;

          Eigen::Vector3d c = lsa->getGlobalCollisionBodyTransform() * center;
          contact.normal = c - pos;
          if (contact.normal.squaredNorm() > std::numeric_limits<double>::epsilon())
          {
            contact.normal.normalize();
          }
          contact.depth = -dist;
          contact.body_name_1 = *link_a->name_;
          contact.body_name_2 = *link_b->name_;
          contact.body_type_1 = BodyTypes::ROBOT_LINK;
          contact.body_type_2 = BodyTypes::ROBOT_LINK;

          // generate a DFContact for debugging?
          if (work.df_contacts_)
          {
            work.df_contacts_->resize(work.df_contacts_->size()+1);
            dfcontact = &work.df_contacts_->back();
            dfcontact->copyFrom(contact);     // This clears all fields.
            dfcontact->sdf_1 = &link_a->df_;
            dfcontact->sphere_center_2 = c;
            dfcontact->sphere_radius_2 = sphere_radii_[i];
          }
          

          if (acm_condition)
          {
            if (acm_condition(contact))
            {
              dfcontact->eliminated_by_acm_function = true;
              continue;
            }
          }

          work.res_->collision = true;
          setCloseDistance(work, dist);

          // contact occurred
          if (work.req_->contacts)
          {
            if (!pair_contacts)
            {
              pair_contacts = &work.res_->contacts[std::make_pair(*link_a->name_, *link_b->name_)];
              pair_contacts->push_back(contact);
            }
            else
            {
              if (contact.depth > pair_contacts->front().depth)
                std::swap(contact, pair_contacts->front());

              if (pair_contacts->size() < work.req_->max_contacts_per_pair)
              {
                pair_contacts->push_back(contact);
                work.res_->contact_count++;
                if (work.res_->contact_count >= work.req_->max_contacts)
                  return;
              }
            }
          }
        }
        else
        {
          work.res_->collision = true;
          setCloseDistance(work, dist);
        }
      }
      next_link:
      (void)0;
    }
  }
}



void collision_detection::CollisionRobotDistanceField::checkSelfCollisionUsingIntraDF(
    WorkArea& work) const
{
  checkSelfCollisionUsingIntraDFLoop(work, &all_links_.front());
}
