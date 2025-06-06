// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#pragma once

#include <memory>
#include <utility>

#include "db/drObj/drRef.h"
#include "db/tech/frViaDef.h"
#include "dr/FlexMazeTypes.h"

namespace drt {

class drNet;
class frVia;

class drVia : public drRef
{
 public:
  // constructors
  drVia() = default;
  drVia(const frViaDef* in) : viaDef_(in) {}
  drVia(const drVia& in) = default;
  drVia(const frVia& in);
  // getters
  const frViaDef* getViaDef() const { return viaDef_; }
  Rect getLayer1BBox() const
  {
    Rect box;
    box.mergeInit();
    for (auto& fig : viaDef_->getLayer1Figs()) {
      box.merge(fig->getBBox());
    }
    getTransform().apply(box);
    return box;
  }
  Rect getCutBBox() const
  {
    Rect box;
    box.mergeInit();
    for (auto& fig : viaDef_->getCutFigs()) {
      box.merge(fig->getBBox());
    }
    getTransform().apply(box);
    return box;
  }
  Rect getLayer2BBox() const
  {
    Rect box;
    box.mergeInit();
    for (auto& fig : viaDef_->getLayer2Figs()) {
      box.merge(fig->getBBox());
    }
    getTransform().apply(box);
    return box;
  }
  // setters
  void setViaDef(const frViaDef* in) { viaDef_ = in; }
  // others
  frBlockObjectEnum typeId() const override { return drcVia; }

  /* from frRef
   * getOrient
   * setOrient
   * getOrigin
   * setOrigin
   * getTransform
   * setTransform
   */

  dbOrientType getOrient() const override { return dbOrientType(); }
  void setOrient(const dbOrientType& tmpOrient) override { ; }
  Point getOrigin() const override { return origin_; }
  void setOrigin(const Point& tmpPoint) override { origin_ = tmpPoint; }
  dbTransform getTransform() const override { return dbTransform(origin_); }
  void setTransform(const dbTransform& xformIn) override {}

  /* from frPinFig
   * hasPin
   * getPin
   * addToPin
   * removeFromPin
   */
  bool hasPin() const override
  {
    return (owner_) && (owner_->typeId() == drcPin);
  }
  drPin* getPin() const override { return reinterpret_cast<drPin*>(owner_); }
  void addToPin(drPin* in) override
  {
    owner_ = reinterpret_cast<drBlockObject*>(in);
  }
  void removeFromPin() override { owner_ = nullptr; }

  /* from frConnFig
   * hasNet
   * getNet
   * addToNet
   * removeFromNet
   */
  bool hasNet() const override
  {
    return (owner_) && (owner_->typeId() == drcNet);
  }
  drNet* getNet() const override { return reinterpret_cast<drNet*>(owner_); }
  void addToNet(drNet* in) override
  {
    owner_ = reinterpret_cast<drBlockObject*>(in);
  }
  void removeFromNet() override { owner_ = nullptr; }

  /* from frFig
   * getBBox
   * move
   * overlaps
   */

  Rect getBBox() const override;
  bool hasMazeIdx() const { return (!beginMazeIdx_.empty()); }
  std::pair<FlexMazeIdx, FlexMazeIdx> getMazeIdx() const
  {
    return {beginMazeIdx_, endMazeIdx_};
  }
  void setMazeIdx(const FlexMazeIdx& bi, const FlexMazeIdx& ei)
  {
    beginMazeIdx_.set(bi);
    endMazeIdx_.set(ei);
  }

  void setTapered(bool t) { tapered_ = t; }

  bool isTapered() const { return tapered_; }

  bool isBottomConnected() const { return bottomConnected_; }
  bool isTopConnected() const { return topConnected_; }
  void setBottomConnected(bool c) { bottomConnected_ = c; }
  void setTopConnected(bool c) { topConnected_ = c; }
  void setIsLonely(bool in) { isLonely_ = in; }
  bool isLonely() const { return isLonely_; }

 protected:
  Point origin_;
  const frViaDef* viaDef_{nullptr};
  drBlockObject* owner_{nullptr};
  FlexMazeIdx beginMazeIdx_;
  FlexMazeIdx endMazeIdx_;
  bool tapered_{false};
  bool bottomConnected_{false};
  bool topConnected_{false};
  bool isLonely_{false};

  template <class Archive>
  void serialize(Archive& ar, unsigned int version);

  friend class boost::serialization::access;
};

}  // namespace drt
