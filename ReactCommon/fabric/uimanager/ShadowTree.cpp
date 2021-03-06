// Copyright (c) Facebook, Inc. and its affiliates.

// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ShadowTree.h"

#include <react/core/LayoutContext.h>
#include <react/core/LayoutPrimitives.h>
#include <react/debug/SystraceSection.h>
#include <react/mounting/Differentiator.h>
#include <react/mounting/ShadowViewMutation.h>

#include "ShadowTreeDelegate.h"

namespace facebook {
namespace react {

ShadowTree::ShadowTree(
    SurfaceId surfaceId,
    const LayoutConstraints &layoutConstraints,
    const LayoutContext &layoutContext)
    : surfaceId_(surfaceId) {
  const auto noopEventEmitter = std::make_shared<const ViewEventEmitter>(
      nullptr, -1, std::shared_ptr<const EventDispatcher>());

  const auto props = std::make_shared<const RootProps>(
      *RootShadowNode::defaultSharedProps(), layoutConstraints, layoutContext);

  rootShadowNode_ = std::make_shared<RootShadowNode>(
      ShadowNodeFragment{
          .tag = surfaceId,
          .rootTag = surfaceId,
          .props = props,
          .eventEmitter = noopEventEmitter,
      },
      nullptr);
}

ShadowTree::~ShadowTree() {
  complete(std::make_shared<SharedShadowNodeList>(SharedShadowNodeList{}));
}

Tag ShadowTree::getSurfaceId() const {
  return surfaceId_;
}

SharedRootShadowNode ShadowTree::getRootShadowNode() const {
  std::lock_guard<std::recursive_mutex> lock(commitMutex_);
  return rootShadowNode_;
}

void ShadowTree::synchronize(std::function<void(void)> function) const {
  std::lock_guard<std::recursive_mutex> lock(commitMutex_);
  function();
}

#pragma mark - Layout

Size ShadowTree::measure(
    const LayoutConstraints &layoutConstraints,
    const LayoutContext &layoutContext) const {
  auto newRootShadowNode = cloneRootShadowNode(
      getRootShadowNode(), layoutConstraints, layoutContext);
  newRootShadowNode->layout();
  return newRootShadowNode->getLayoutMetrics().frame.size;
}

bool ShadowTree::constraintLayout(
    const LayoutConstraints &layoutConstraints,
    const LayoutContext &layoutContext) const {
  auto oldRootShadowNode = getRootShadowNode();
  auto newRootShadowNode =
      cloneRootShadowNode(oldRootShadowNode, layoutConstraints, layoutContext);
  return complete(oldRootShadowNode, newRootShadowNode);
}

#pragma mark - Commiting

UnsharedRootShadowNode ShadowTree::cloneRootShadowNode(
    const SharedRootShadowNode &oldRootShadowNode,
    const LayoutConstraints &layoutConstraints,
    const LayoutContext &layoutContext) const {
  auto props = std::make_shared<const RootProps>(
      *oldRootShadowNode->getProps(), layoutConstraints, layoutContext);
  auto newRootShadowNode = std::make_shared<RootShadowNode>(
      *oldRootShadowNode, ShadowNodeFragment{.props = props});
  return newRootShadowNode;
}

bool ShadowTree::complete(
    const SharedShadowNodeUnsharedList &rootChildNodes) const {
  auto oldRootShadowNode = getRootShadowNode();
  auto newRootShadowNode = std::make_shared<RootShadowNode>(
      *oldRootShadowNode,
      ShadowNodeFragment{.children =
                             SharedShadowNodeSharedList(rootChildNodes)});

  return complete(oldRootShadowNode, newRootShadowNode);
}

bool ShadowTree::completeByReplacingShadowNode(
    const SharedShadowNode &oldShadowNode,
    const SharedShadowNode &newShadowNode) const {
  auto rootShadowNode = getRootShadowNode();
  std::vector<std::reference_wrapper<const ShadowNode>> ancestors;
  oldShadowNode->constructAncestorPath(*rootShadowNode, ancestors);

  if (ancestors.size() == 0) {
    return false;
  }

  auto oldChild = oldShadowNode;
  auto newChild = newShadowNode;

  SharedShadowNodeUnsharedList sharedChildren;

  for (const auto &ancestor : ancestors) {
    auto children = ancestor.get().getChildren();
    std::replace(children.begin(), children.end(), oldChild, newChild);

    sharedChildren = std::make_shared<SharedShadowNodeList>(children);

    oldChild = ancestor.get().shared_from_this();
    newChild = oldChild->clone(ShadowNodeFragment{.children = sharedChildren});
  }

  return complete(sharedChildren);
}

bool ShadowTree::complete(
    const SharedRootShadowNode &oldRootShadowNode,
    const UnsharedRootShadowNode &newRootShadowNode) const {
  SystraceSection s("ShadowTree::complete");
  newRootShadowNode->layout();
  newRootShadowNode->sealRecursive();

  auto mutations =
      calculateShadowViewMutations(*oldRootShadowNode, *newRootShadowNode);

  if (!commit(oldRootShadowNode, newRootShadowNode, mutations)) {
    return false;
  }

  emitLayoutEvents(mutations);

  if (delegate_) {
    delegate_->shadowTreeDidCommit(*this, mutations);
  }

  return true;
}

bool ShadowTree::commit(
    const SharedRootShadowNode &oldRootShadowNode,
    const SharedRootShadowNode &newRootShadowNode,
    const ShadowViewMutationList &mutations) const {
  SystraceSection s("ShadowTree::commit");
  std::lock_guard<std::recursive_mutex> lock(commitMutex_);

  if (oldRootShadowNode != rootShadowNode_) {
    return false;
  }

  rootShadowNode_ = newRootShadowNode;

  toggleEventEmitters(mutations);

  return true;
}

void ShadowTree::emitLayoutEvents(
    const ShadowViewMutationList &mutations) const {
  SystraceSection s("ShadowTree::emitLayoutEvents");

  for (const auto &mutation : mutations) {
    // Only `Insert` and `Update` mutations can affect layout metrics.
    if (mutation.type != ShadowViewMutation::Insert &&
        mutation.type != ShadowViewMutation::Update) {
      continue;
    }

    const auto viewEventEmitter =
        std::dynamic_pointer_cast<const ViewEventEmitter>(
            mutation.newChildShadowView.eventEmitter);

    // Checking if particular shadow node supports `onLayout` event (part of
    // `ViewEventEmitter`).
    if (!viewEventEmitter) {
      continue;
    }

    // Checking if the `onLayout` event was requested for the particular Shadow
    // Node.
    const auto viewProps = std::dynamic_pointer_cast<const ViewProps>(
        mutation.newChildShadowView.props);
    if (viewProps && !viewProps->onLayout) {
      continue;
    }

    // In case if we have `oldChildShadowView`, checking that layout metrics
    // have changed.
    if (mutation.type != ShadowViewMutation::Update &&
        mutation.oldChildShadowView.layoutMetrics ==
            mutation.newChildShadowView.layoutMetrics) {
      continue;
    }

    viewEventEmitter->onLayout(mutation.newChildShadowView.layoutMetrics);
  }
}

void ShadowTree::toggleEventEmitters(
    const ShadowViewMutationList &mutations) const {
  std::lock_guard<std::recursive_mutex> lock(EventEmitter::DispatchMutex());

  for (const auto &mutation : mutations) {
    if (mutation.type == ShadowViewMutation::Create) {
      mutation.newChildShadowView.eventEmitter->enable();
    }
  }

  for (const auto &mutation : mutations) {
    if (mutation.type == ShadowViewMutation::Delete) {
      mutation.oldChildShadowView.eventEmitter->disable();
    }
  }
}

#pragma mark - Delegate

void ShadowTree::setDelegate(ShadowTreeDelegate const *delegate) {
  delegate_ = delegate;
}

ShadowTreeDelegate const *ShadowTree::getDelegate() const {
  return delegate_;
}

} // namespace react
} // namespace facebook
