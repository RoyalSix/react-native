/**
 * Copyright (c) 2014-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <string.h>

#include "CSSLayout.h"
#include "CSSNodeList.h"

#ifdef _MSC_VER
#include <float.h>
#define isnan _isnan

/* define fmaxf if < VC12 */
#if _MSC_VER < 1800
__forceinline const float fmaxf(const float a, const float b) {
  return (a > b) ? a : b;
}
#endif
#endif

typedef struct CSSCachedMeasurement {
  float availableWidth;
  float availableHeight;
  CSSMeasureMode widthMeasureMode;
  CSSMeasureMode heightMeasureMode;

  float computedWidth;
  float computedHeight;
} CSSCachedMeasurement;

// This value was chosen based on empiracle data. Even the most complicated
// layouts should not require more than 16 entries to fit within the cache.
enum { CSS_MAX_CACHED_RESULT_COUNT = 16 };

typedef struct CSSLayout {
  float position[4];
  float dimensions[2];
  CSSDirection direction;

  float computedFlexBasis;

  // Instead of recomputing the entire layout every single time, we
  // cache some information to break early when nothing changed
  uint32_t generationCount;
  CSSDirection lastParentDirection;

  uint32_t nextCachedMeasurementsIndex;
  CSSCachedMeasurement cachedMeasurements[CSS_MAX_CACHED_RESULT_COUNT];
  float measuredDimensions[2];

  CSSCachedMeasurement cachedLayout;
} CSSLayout;

typedef struct CSSStyle {
  CSSDirection direction;
  CSSFlexDirection flexDirection;
  CSSJustify justifyContent;
  CSSAlign alignContent;
  CSSAlign alignItems;
  CSSAlign alignSelf;
  CSSPositionType positionType;
  CSSWrapType flexWrap;
  CSSOverflow overflow;
  float flexGrow;
  float flexShrink;
  float flexBasis;
  float margin[CSSEdgeCount];
  float position[CSSEdgeCount];
  float padding[CSSEdgeCount];
  float border[CSSEdgeCount];
  float dimensions[2];
  float minDimensions[2];
  float maxDimensions[2];
} CSSStyle;

typedef struct CSSNode {
  CSSStyle style;
  CSSLayout layout;
  uint32_t lineIndex;
  bool hasNewLayout;
  bool isTextNode;
  CSSNodeRef parent;
  CSSNodeListRef children;
  bool isDirty;

  struct CSSNode *nextChild;

  CSSMeasureFunc measure;
  CSSPrintFunc print;
  void *context;
} CSSNode;

static void _CSSNodeMarkDirty(const CSSNodeRef node);

static CSSLogger gLogger = &printf;

static float computedEdgeValue(const float edges[CSSEdgeCount],
                               const CSSEdge edge,
                               const float defaultValue) {
  CSS_ASSERT(edge <= CSSEdgeEnd, "Cannot get computed value of multi-edge shorthands");

  if (!CSSValueIsUndefined(edges[edge])) {
    return edges[edge];
  }

  if ((edge == CSSEdgeTop || edge == CSSEdgeBottom) &&
      !CSSValueIsUndefined(edges[CSSEdgeVertical])) {
    return edges[CSSEdgeVertical];
  }

  if ((edge == CSSEdgeLeft || edge == CSSEdgeRight || edge == CSSEdgeStart || edge == CSSEdgeEnd) &&
      !CSSValueIsUndefined(edges[CSSEdgeHorizontal])) {
    return edges[CSSEdgeHorizontal];
  }

  if (!CSSValueIsUndefined(edges[CSSEdgeAll])) {
    return edges[CSSEdgeAll];
  }

  if (edge == CSSEdgeStart || edge == CSSEdgeEnd) {
    return CSSUndefined;
  }

  return defaultValue;
}

static int32_t gNodeInstanceCount = 0;

CSSNodeRef CSSNodeNew(void) {
  const CSSNodeRef node = calloc(1, sizeof(CSSNode));
  CSS_ASSERT(node, "Could not allocate memory for node");
  gNodeInstanceCount++;

  CSSNodeInit(node);
  return node;
}

void CSSNodeFree(const CSSNodeRef node) {
  CSSNodeListFree(node->children);
  free(node);
  gNodeInstanceCount--;
}

void CSSNodeFreeRecursive(const CSSNodeRef root) {
  while (CSSNodeChildCount(root) > 0) {
    const CSSNodeRef child = CSSNodeGetChild(root, 0);
    CSSNodeRemoveChild(root, child);
    CSSNodeFreeRecursive(child);
  }
  CSSNodeFree(root);
}

int32_t CSSNodeGetInstanceCount(void) {
  return gNodeInstanceCount;
}

void CSSNodeInit(const CSSNodeRef node) {
  node->parent = NULL;
  node->children = CSSNodeListNew(4);
  node->hasNewLayout = true;
  node->isDirty = false;

  node->style.flexGrow = 0;
  node->style.flexShrink = 0;
  node->style.flexBasis = CSSUndefined;

  node->style.alignItems = CSSAlignStretch;
  node->style.alignContent = CSSAlignFlexStart;

  node->style.direction = CSSDirectionInherit;
  node->style.flexDirection = CSSFlexDirectionColumn;

  node->style.overflow = CSSOverflowVisible;

  // Some of the fields default to undefined and not 0
  node->style.dimensions[CSSDimensionWidth] = CSSUndefined;
  node->style.dimensions[CSSDimensionHeight] = CSSUndefined;

  node->style.minDimensions[CSSDimensionWidth] = CSSUndefined;
  node->style.minDimensions[CSSDimensionHeight] = CSSUndefined;

  node->style.maxDimensions[CSSDimensionWidth] = CSSUndefined;
  node->style.maxDimensions[CSSDimensionHeight] = CSSUndefined;

  for (CSSEdge edge = CSSEdgeLeft; edge < CSSEdgeCount; edge++) {
    node->style.position[edge] = CSSUndefined;
    node->style.margin[edge] = CSSUndefined;
    node->style.padding[edge] = CSSUndefined;
    node->style.border[edge] = CSSUndefined;
  }

  node->layout.dimensions[CSSDimensionWidth] = CSSUndefined;
  node->layout.dimensions[CSSDimensionHeight] = CSSUndefined;

  // Such that the comparison is always going to be false
  node->layout.lastParentDirection = (CSSDirection) -1;
  node->layout.nextCachedMeasurementsIndex = 0;
  node->layout.computedFlexBasis = CSSUndefined;

  node->layout.measuredDimensions[CSSDimensionWidth] = CSSUndefined;
  node->layout.measuredDimensions[CSSDimensionHeight] = CSSUndefined;
  node->layout.cachedLayout.widthMeasureMode = (CSSMeasureMode) -1;
  node->layout.cachedLayout.heightMeasureMode = (CSSMeasureMode) -1;
}

static void _CSSNodeMarkDirty(const CSSNodeRef node) {
  if (!node->isDirty) {
    node->isDirty = true;
    node->layout.computedFlexBasis = CSSUndefined;
    if (node->parent) {
      _CSSNodeMarkDirty(node->parent);
    }
  }
}

void CSSNodeInsertChild(const CSSNodeRef node, const CSSNodeRef child, const uint32_t index) {
  CSS_ASSERT(child->parent == NULL, "Child already has a parent, it must be removed first.");
  CSSNodeListInsert(node->children, child, index);
  child->parent = node;
  _CSSNodeMarkDirty(node);
}

void CSSNodeRemoveChild(const CSSNodeRef node, const CSSNodeRef child) {
  CSSNodeListDelete(node->children, child);
  child->parent = NULL;
  _CSSNodeMarkDirty(node);
}

CSSNodeRef CSSNodeGetChild(const CSSNodeRef node, const uint32_t index) {
  return CSSNodeListGet(node->children, index);
}

uint32_t CSSNodeChildCount(const CSSNodeRef node) {
  return CSSNodeListCount(node->children);
}

void CSSNodeMarkDirty(const CSSNodeRef node) {
  CSS_ASSERT(node->measure != NULL || CSSNodeChildCount(node) > 0,
             "Only leaf nodes with custom measure functions"
             "should manually mark themselves as dirty");
  _CSSNodeMarkDirty(node);
}

bool CSSNodeIsDirty(const CSSNodeRef node) {
  return node->isDirty;
}

void CSSNodeStyleSetFlex(const CSSNodeRef node, const float flex) {
  if (CSSValueIsUndefined(flex) || flex == 0) {
    CSSNodeStyleSetFlexGrow(node, 0);
    CSSNodeStyleSetFlexShrink(node, 0);
    CSSNodeStyleSetFlexBasis(node, CSSUndefined);
  } else if (flex > 0) {
    CSSNodeStyleSetFlexGrow(node, flex);
    CSSNodeStyleSetFlexShrink(node, 0);
    CSSNodeStyleSetFlexBasis(node, 0);
  } else {
    CSSNodeStyleSetFlexGrow(node, 0);
    CSSNodeStyleSetFlexShrink(node, -flex);
    CSSNodeStyleSetFlexBasis(node, CSSUndefined);
  }
}

float CSSNodeStyleGetFlex(const CSSNodeRef node) {
  if (node->style.flexGrow > 0) {
    return node->style.flexGrow;
  } else if (node->style.flexShrink > 0) {
    return -node->style.flexShrink;
  }

  return 0;
}

#define CSS_NODE_PROPERTY_IMPL(type, name, paramName, instanceName) \
  void CSSNodeSet##name(const CSSNodeRef node, type paramName) {    \
    node->instanceName = paramName;                                 \
  }                                                                 \
                                                                    \
  type CSSNodeGet##name(const CSSNodeRef node) {                    \
    return node->instanceName;                                      \
  }

#define CSS_NODE_STYLE_PROPERTY_IMPL(type, name, paramName, instanceName)   \
  void CSSNodeStyleSet##name(const CSSNodeRef node, const type paramName) { \
    if (node->style.instanceName != paramName) {                            \
      node->style.instanceName = paramName;                                 \
      _CSSNodeMarkDirty(node);                                              \
    }                                                                       \
  }                                                                         \
                                                                            \
  type CSSNodeStyleGet##name(const CSSNodeRef node) {                       \
    return node->style.instanceName;                                        \
  }

#define CSS_NODE_STYLE_EDGE_PROPERTY_IMPL(type, name, paramName, instanceName, defaultValue)    \
  void CSSNodeStyleSet##name(const CSSNodeRef node, const CSSEdge edge, const type paramName) { \
    if (node->style.instanceName[edge] != paramName) {                                          \
      node->style.instanceName[edge] = paramName;                                               \
      _CSSNodeMarkDirty(node);                                                                  \
    }                                                                                           \
  }                                                                                             \
                                                                                                \
  type CSSNodeStyleGet##name(const CSSNodeRef node, const CSSEdge edge) {                       \
    return computedEdgeValue(node->style.instanceName, edge, defaultValue);                     \
  }

#define CSS_NODE_LAYOUT_PROPERTY_IMPL(type, name, instanceName) \
  type CSSNodeLayoutGet##name(const CSSNodeRef node) {          \
    return node->layout.instanceName;                           \
  }

CSS_NODE_PROPERTY_IMPL(void *, Context, context, context);
CSS_NODE_PROPERTY_IMPL(CSSMeasureFunc, MeasureFunc, measureFunc, measure);
CSS_NODE_PROPERTY_IMPL(CSSPrintFunc, PrintFunc, printFunc, print);
CSS_NODE_PROPERTY_IMPL(bool, IsTextnode, isTextNode, isTextNode);
CSS_NODE_PROPERTY_IMPL(bool, HasNewLayout, hasNewLayout, hasNewLayout);

CSS_NODE_STYLE_PROPERTY_IMPL(CSSDirection, Direction, direction, direction);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSFlexDirection, FlexDirection, flexDirection, flexDirection);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSJustify, JustifyContent, justifyContent, justifyContent);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSAlign, AlignContent, alignContent, alignContent);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSAlign, AlignItems, alignItems, alignItems);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSAlign, AlignSelf, alignSelf, alignSelf);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSPositionType, PositionType, positionType, positionType);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSWrapType, FlexWrap, flexWrap, flexWrap);
CSS_NODE_STYLE_PROPERTY_IMPL(CSSOverflow, Overflow, overflow, overflow);
CSS_NODE_STYLE_PROPERTY_IMPL(float, FlexGrow, flexGrow, flexGrow);
CSS_NODE_STYLE_PROPERTY_IMPL(float, FlexShrink, flexShrink, flexShrink);
CSS_NODE_STYLE_PROPERTY_IMPL(float, FlexBasis, flexBasis, flexBasis);

CSS_NODE_STYLE_EDGE_PROPERTY_IMPL(float, Position, position, position, CSSUndefined);
CSS_NODE_STYLE_EDGE_PROPERTY_IMPL(float, Margin, margin, margin, 0);
CSS_NODE_STYLE_EDGE_PROPERTY_IMPL(float, Padding, padding, padding, 0);
CSS_NODE_STYLE_EDGE_PROPERTY_IMPL(float, Border, border, border, 0);

CSS_NODE_STYLE_PROPERTY_IMPL(float, Width, width, dimensions[CSSDimensionWidth]);
CSS_NODE_STYLE_PROPERTY_IMPL(float, Height, height, dimensions[CSSDimensionHeight]);
CSS_NODE_STYLE_PROPERTY_IMPL(float, MinWidth, minWidth, minDimensions[CSSDimensionWidth]);
CSS_NODE_STYLE_PROPERTY_IMPL(float, MinHeight, minHeight, minDimensions[CSSDimensionHeight]);
CSS_NODE_STYLE_PROPERTY_IMPL(float, MaxWidth, maxWidth, maxDimensions[CSSDimensionWidth]);
CSS_NODE_STYLE_PROPERTY_IMPL(float, MaxHeight, maxHeight, maxDimensions[CSSDimensionHeight]);

CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Left, position[CSSEdgeLeft]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Top, position[CSSEdgeTop]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Right, position[CSSEdgeRight]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Bottom, position[CSSEdgeBottom]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Width, dimensions[CSSDimensionWidth]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(float, Height, dimensions[CSSDimensionHeight]);
CSS_NODE_LAYOUT_PROPERTY_IMPL(CSSDirection, Direction, direction);

uint32_t gCurrentGenerationCount = 0;

bool layoutNodeInternal(const CSSNodeRef node,
                        const float availableWidth,
                        const float availableHeight,
                        const CSSDirection parentDirection,
                        const CSSMeasureMode widthMeasureMode,
                        const CSSMeasureMode heightMeasureMode,
                        const bool performLayout,
                        const char *reason);

bool CSSValueIsUndefined(const float value) {
  return isnan(value);
}

static bool eq(const float a, const float b) {
  if (CSSValueIsUndefined(a)) {
    return CSSValueIsUndefined(b);
  }
  return fabs(a - b) < 0.0001;
}

static void indent(const uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    gLogger("  ");
  }
}

static void printNumberIfNotZero(const char *str, const float number) {
  if (!eq(number, 0)) {
    gLogger("%s: %g, ", str, number);
  }
}

static void printNumberIfNotUndefined(const char *str, const float number) {
  if (!CSSValueIsUndefined(number)) {
    gLogger("%s: %g, ", str, number);
  }
}

static bool eqFour(const float four[4]) {
  return eq(four[0], four[1]) && eq(four[0], four[2]) && eq(four[0], four[3]);
}

static void _CSSNodePrint(const CSSNodeRef node,
                          const CSSPrintOptions options,
                          const uint32_t level) {
  indent(level);
  gLogger("{");

  if (node->print) {
    node->print(node->context);
  }

  if (options & CSSPrintOptionsLayout) {
    gLogger("layout: {");
    gLogger("width: %g, ", node->layout.dimensions[CSSDimensionWidth]);
    gLogger("height: %g, ", node->layout.dimensions[CSSDimensionHeight]);
    gLogger("top: %g, ", node->layout.position[CSSEdgeTop]);
    gLogger("left: %g", node->layout.position[CSSEdgeLeft]);
    gLogger("}, ");
  }

  if (options & CSSPrintOptionsStyle) {
    if (node->style.flexDirection == CSSFlexDirectionColumn) {
      gLogger("flexDirection: 'column', ");
    } else if (node->style.flexDirection == CSSFlexDirectionColumnReverse) {
      gLogger("flexDirection: 'column-reverse', ");
    } else if (node->style.flexDirection == CSSFlexDirectionRow) {
      gLogger("flexDirection: 'row', ");
    } else if (node->style.flexDirection == CSSFlexDirectionRowReverse) {
      gLogger("flexDirection: 'row-reverse', ");
    }

    if (node->style.justifyContent == CSSJustifyCenter) {
      gLogger("justifyContent: 'center', ");
    } else if (node->style.justifyContent == CSSJustifyFlexEnd) {
      gLogger("justifyContent: 'flex-end', ");
    } else if (node->style.justifyContent == CSSJustifySpaceAround) {
      gLogger("justifyContent: 'space-around', ");
    } else if (node->style.justifyContent == CSSJustifySpaceBetween) {
      gLogger("justifyContent: 'space-between', ");
    }

    if (node->style.alignItems == CSSAlignCenter) {
      gLogger("alignItems: 'center', ");
    } else if (node->style.alignItems == CSSAlignFlexEnd) {
      gLogger("alignItems: 'flex-end', ");
    } else if (node->style.alignItems == CSSAlignStretch) {
      gLogger("alignItems: 'stretch', ");
    }

    if (node->style.alignContent == CSSAlignCenter) {
      gLogger("alignContent: 'center', ");
    } else if (node->style.alignContent == CSSAlignFlexEnd) {
      gLogger("alignContent: 'flex-end', ");
    } else if (node->style.alignContent == CSSAlignStretch) {
      gLogger("alignContent: 'stretch', ");
    }

    if (node->style.alignSelf == CSSAlignFlexStart) {
      gLogger("alignSelf: 'flex-start', ");
    } else if (node->style.alignSelf == CSSAlignCenter) {
      gLogger("alignSelf: 'center', ");
    } else if (node->style.alignSelf == CSSAlignFlexEnd) {
      gLogger("alignSelf: 'flex-end', ");
    } else if (node->style.alignSelf == CSSAlignStretch) {
      gLogger("alignSelf: 'stretch', ");
    }

    printNumberIfNotUndefined("flexGrow", node->style.flexGrow);
    printNumberIfNotUndefined("flexShrink", node->style.flexShrink);
    printNumberIfNotUndefined("flexBasis", node->style.flexBasis);

    if (node->style.overflow == CSSOverflowHidden) {
      gLogger("overflow: 'hidden', ");
    } else if (node->style.overflow == CSSOverflowVisible) {
      gLogger("overflow: 'visible', ");
    } else if (node->style.overflow == CSSOverflowScroll) {
      gLogger("overflow: 'scroll', ");
    }

    if (eqFour(node->style.margin)) {
      printNumberIfNotZero("margin", computedEdgeValue(node->style.margin, CSSEdgeLeft, 0));
    } else {
      printNumberIfNotZero("marginLeft", computedEdgeValue(node->style.margin, CSSEdgeLeft, 0));
      printNumberIfNotZero("marginRight", computedEdgeValue(node->style.margin, CSSEdgeRight, 0));
      printNumberIfNotZero("marginTop", computedEdgeValue(node->style.margin, CSSEdgeTop, 0));
      printNumberIfNotZero("marginBottom", computedEdgeValue(node->style.margin, CSSEdgeBottom, 0));
      printNumberIfNotZero("marginStart", computedEdgeValue(node->style.margin, CSSEdgeStart, 0));
      printNumberIfNotZero("marginEnd", computedEdgeValue(node->style.margin, CSSEdgeEnd, 0));
    }

    if (eqFour(node->style.padding)) {
      printNumberIfNotZero("padding", computedEdgeValue(node->style.padding, CSSEdgeLeft, 0));
    } else {
      printNumberIfNotZero("paddingLeft", computedEdgeValue(node->style.padding, CSSEdgeLeft, 0));
      printNumberIfNotZero("paddingRight", computedEdgeValue(node->style.padding, CSSEdgeRight, 0));
      printNumberIfNotZero("paddingTop", computedEdgeValue(node->style.padding, CSSEdgeTop, 0));
      printNumberIfNotZero("paddingBottom",
                           computedEdgeValue(node->style.padding, CSSEdgeBottom, 0));
      printNumberIfNotZero("paddingStart", computedEdgeValue(node->style.padding, CSSEdgeStart, 0));
      printNumberIfNotZero("paddingEnd", computedEdgeValue(node->style.padding, CSSEdgeEnd, 0));
    }

    if (eqFour(node->style.border)) {
      printNumberIfNotZero("borderWidth", computedEdgeValue(node->style.border, CSSEdgeLeft, 0));
    } else {
      printNumberIfNotZero("borderLeftWidth",
                           computedEdgeValue(node->style.border, CSSEdgeLeft, 0));
      printNumberIfNotZero("borderRightWidth",
                           computedEdgeValue(node->style.border, CSSEdgeRight, 0));
      printNumberIfNotZero("borderTopWidth", computedEdgeValue(node->style.border, CSSEdgeTop, 0));
      printNumberIfNotZero("borderBottomWidth",
                           computedEdgeValue(node->style.border, CSSEdgeBottom, 0));
      printNumberIfNotZero("borderStartWidth",
                           computedEdgeValue(node->style.border, CSSEdgeStart, 0));
      printNumberIfNotZero("borderEndWidth", computedEdgeValue(node->style.border, CSSEdgeEnd, 0));
    }

    printNumberIfNotUndefined("width", node->style.dimensions[CSSDimensionWidth]);
    printNumberIfNotUndefined("height", node->style.dimensions[CSSDimensionHeight]);
    printNumberIfNotUndefined("maxWidth", node->style.maxDimensions[CSSDimensionWidth]);
    printNumberIfNotUndefined("maxHeight", node->style.maxDimensions[CSSDimensionHeight]);
    printNumberIfNotUndefined("minWidth", node->style.minDimensions[CSSDimensionWidth]);
    printNumberIfNotUndefined("minHeight", node->style.minDimensions[CSSDimensionHeight]);

    if (node->style.positionType == CSSPositionTypeAbsolute) {
      gLogger("position: 'absolute', ");
    }

    printNumberIfNotUndefined("left",
                              computedEdgeValue(node->style.position, CSSEdgeLeft, CSSUndefined));
    printNumberIfNotUndefined("right",
                              computedEdgeValue(node->style.position, CSSEdgeRight, CSSUndefined));
    printNumberIfNotUndefined("top",
                              computedEdgeValue(node->style.position, CSSEdgeTop, CSSUndefined));
    printNumberIfNotUndefined("bottom",
                              computedEdgeValue(node->style.position, CSSEdgeBottom, CSSUndefined));
  }

  const uint32_t childCount = CSSNodeListCount(node->children);
  if (options & CSSPrintOptionsChildren && childCount > 0) {
    gLogger("children: [\n");
    for (uint32_t i = 0; i < childCount; i++) {
      _CSSNodePrint(CSSNodeGetChild(node, i), options, level + 1);
    }
    indent(level);
    gLogger("]},\n");
  } else {
    gLogger("},\n");
  }
}

void CSSNodePrint(const CSSNodeRef node, const CSSPrintOptions options) {
  _CSSNodePrint(node, options, 0);
}

static const CSSEdge leading[4] = {
        [CSSFlexDirectionColumn] = CSSEdgeTop,
        [CSSFlexDirectionColumnReverse] = CSSEdgeBottom,
        [CSSFlexDirectionRow] = CSSEdgeLeft,
        [CSSFlexDirectionRowReverse] = CSSEdgeRight,
};
static const CSSEdge trailing[4] = {
        [CSSFlexDirectionColumn] = CSSEdgeBottom,
        [CSSFlexDirectionColumnReverse] = CSSEdgeTop,
        [CSSFlexDirectionRow] = CSSEdgeRight,
        [CSSFlexDirectionRowReverse] = CSSEdgeLeft,
};
static const CSSEdge pos[4] = {
        [CSSFlexDirectionColumn] = CSSEdgeTop,
        [CSSFlexDirectionColumnReverse] = CSSEdgeBottom,
        [CSSFlexDirectionRow] = CSSEdgeLeft,
        [CSSFlexDirectionRowReverse] = CSSEdgeRight,
};
static const CSSDimension dim[4] = {
        [CSSFlexDirectionColumn] = CSSDimensionHeight,
        [CSSFlexDirectionColumnReverse] = CSSDimensionHeight,
        [CSSFlexDirectionRow] = CSSDimensionWidth,
        [CSSFlexDirectionRowReverse] = CSSDimensionWidth,
};

static bool isRowDirection(const CSSFlexDirection flexDirection) {
  return flexDirection == CSSFlexDirectionRow || flexDirection == CSSFlexDirectionRowReverse;
}

static bool isColumnDirection(const CSSFlexDirection flexDirection) {
  return flexDirection == CSSFlexDirectionColumn || flexDirection == CSSFlexDirectionColumnReverse;
}

static float getLeadingMargin(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.margin[CSSEdgeStart])) {
    return node->style.margin[CSSEdgeStart];
  }

  return computedEdgeValue(node->style.margin, leading[axis], 0);
}

static float getTrailingMargin(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.margin[CSSEdgeEnd])) {
    return node->style.margin[CSSEdgeEnd];
  }

  return computedEdgeValue(node->style.margin, trailing[axis], 0);
}

static float getLeadingPadding(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.padding[CSSEdgeStart]) &&
      node->style.padding[CSSEdgeStart] >= 0) {
    return node->style.padding[CSSEdgeStart];
  }

  if (computedEdgeValue(node->style.padding, leading[axis], 0) >= 0) {
    return computedEdgeValue(node->style.padding, leading[axis], 0);
  }

  return 0;
}

static float getTrailingPadding(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.padding[CSSEdgeEnd]) &&
      node->style.padding[CSSEdgeEnd] >= 0) {
    return node->style.padding[CSSEdgeEnd];
  }

  if (computedEdgeValue(node->style.padding, trailing[axis], 0) >= 0) {
    return computedEdgeValue(node->style.padding, trailing[axis], 0);
  }

  return 0;
}

static float getLeadingBorder(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.border[CSSEdgeStart]) &&
      node->style.border[CSSEdgeStart] >= 0) {
    return node->style.border[CSSEdgeStart];
  }

  if (computedEdgeValue(node->style.border, leading[axis], 0) >= 0) {
    return computedEdgeValue(node->style.border, leading[axis], 0);
  }

  return 0;
}

static float getTrailingBorder(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) && !CSSValueIsUndefined(node->style.border[CSSEdgeEnd]) &&
      node->style.border[CSSEdgeEnd] >= 0) {
    return node->style.border[CSSEdgeEnd];
  }

  if (computedEdgeValue(node->style.border, trailing[axis], 0) >= 0) {
    return computedEdgeValue(node->style.border, trailing[axis], 0);
  }

  return 0;
}

static float getLeadingPaddingAndBorder(const CSSNodeRef node, const CSSFlexDirection axis) {
  return getLeadingPadding(node, axis) + getLeadingBorder(node, axis);
}

static float getTrailingPaddingAndBorder(const CSSNodeRef node, const CSSFlexDirection axis) {
  return getTrailingPadding(node, axis) + getTrailingBorder(node, axis);
}

static float getMarginAxis(const CSSNodeRef node, const CSSFlexDirection axis) {
  return getLeadingMargin(node, axis) + getTrailingMargin(node, axis);
}

static float getPaddingAndBorderAxis(const CSSNodeRef node, const CSSFlexDirection axis) {
  return getLeadingPaddingAndBorder(node, axis) + getTrailingPaddingAndBorder(node, axis);
}

static CSSAlign getAlignItem(const CSSNodeRef node, const CSSNodeRef child) {
  if (child->style.alignSelf != CSSAlignAuto) {
    return child->style.alignSelf;
  }
  return node->style.alignItems;
}

static CSSDirection resolveDirection(const CSSNodeRef node, const CSSDirection parentDirection) {
  if (node->style.direction == CSSDirectionInherit) {
    return parentDirection > CSSDirectionInherit ? parentDirection : CSSDirectionLTR;
  } else {
    return node->style.direction;
  }
}

static CSSFlexDirection resolveAxis(const CSSFlexDirection flexDirection,
                                    const CSSDirection direction) {
  if (direction == CSSDirectionRTL) {
    if (flexDirection == CSSFlexDirectionRow) {
      return CSSFlexDirectionRowReverse;
    } else if (flexDirection == CSSFlexDirectionRowReverse) {
      return CSSFlexDirectionRow;
    }
  }

  return flexDirection;
}

static CSSFlexDirection getCrossFlexDirection(const CSSFlexDirection flexDirection,
                                              const CSSDirection direction) {
  if (isColumnDirection(flexDirection)) {
    return resolveAxis(CSSFlexDirectionRow, direction);
  } else {
    return CSSFlexDirectionColumn;
  }
}

static bool isFlex(const CSSNodeRef node) {
  return (node->style.positionType == CSSPositionTypeRelative &&
          (node->style.flexGrow != 0 || node->style.flexShrink != 0));
}

static float getDimWithMargin(const CSSNodeRef node, const CSSFlexDirection axis) {
  return node->layout.measuredDimensions[dim[axis]] + getLeadingMargin(node, axis) +
         getTrailingMargin(node, axis);
}

static bool isStyleDimDefined(const CSSNodeRef node, const CSSFlexDirection axis) {
  const float value = node->style.dimensions[dim[axis]];
  return !CSSValueIsUndefined(value) && value >= 0.0;
}

static bool isLayoutDimDefined(const CSSNodeRef node, const CSSFlexDirection axis) {
  const float value = node->layout.measuredDimensions[dim[axis]];
  return !CSSValueIsUndefined(value) && value >= 0.0;
}

static bool isLeadingPosDefined(const CSSNodeRef node, const CSSFlexDirection axis) {
  return (isRowDirection(axis) &&
          !CSSValueIsUndefined(
              computedEdgeValue(node->style.position, CSSEdgeStart, CSSUndefined))) ||
         !CSSValueIsUndefined(computedEdgeValue(node->style.position, leading[axis], CSSUndefined));
}

static bool isTrailingPosDefined(const CSSNodeRef node, const CSSFlexDirection axis) {
  return (isRowDirection(axis) &&
          !CSSValueIsUndefined(
              computedEdgeValue(node->style.position, CSSEdgeEnd, CSSUndefined))) ||
         !CSSValueIsUndefined(
             computedEdgeValue(node->style.position, trailing[axis], CSSUndefined));
}

static float getLeadingPosition(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) &&
      !CSSValueIsUndefined(computedEdgeValue(node->style.position, CSSEdgeStart, CSSUndefined))) {
    return computedEdgeValue(node->style.position, CSSEdgeStart, CSSUndefined);
  }
  if (!CSSValueIsUndefined(computedEdgeValue(node->style.position, leading[axis], CSSUndefined))) {
    return computedEdgeValue(node->style.position, leading[axis], CSSUndefined);
  }
  return 0;
}

static float getTrailingPosition(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isRowDirection(axis) &&
      !CSSValueIsUndefined(computedEdgeValue(node->style.position, CSSEdgeEnd, CSSUndefined))) {
    return computedEdgeValue(node->style.position, CSSEdgeEnd, CSSUndefined);
  }
  if (!CSSValueIsUndefined(computedEdgeValue(node->style.position, trailing[axis], CSSUndefined))) {
    return computedEdgeValue(node->style.position, trailing[axis], CSSUndefined);
  }
  return 0;
}

static float boundAxisWithinMinAndMax(const CSSNodeRef node,
                                      const CSSFlexDirection axis,
                                      const float value) {
  float min = CSSUndefined;
  float max = CSSUndefined;

  if (isColumnDirection(axis)) {
    min = node->style.minDimensions[CSSDimensionHeight];
    max = node->style.maxDimensions[CSSDimensionHeight];
  } else if (isRowDirection(axis)) {
    min = node->style.minDimensions[CSSDimensionWidth];
    max = node->style.maxDimensions[CSSDimensionWidth];
  }

  float boundValue = value;

  if (!CSSValueIsUndefined(max) && max >= 0.0 && boundValue > max) {
    boundValue = max;
  }

  if (!CSSValueIsUndefined(min) && min >= 0.0 && boundValue < min) {
    boundValue = min;
  }

  return boundValue;
}

// Like boundAxisWithinMinAndMax but also ensures that the value doesn't go
// below the
// padding and border amount.
static float boundAxis(const CSSNodeRef node, const CSSFlexDirection axis, const float value) {
  return fmaxf(boundAxisWithinMinAndMax(node, axis, value), getPaddingAndBorderAxis(node, axis));
}

static void setTrailingPosition(const CSSNodeRef node,
                                const CSSNodeRef child,
                                const CSSFlexDirection axis) {
  const float size = child->layout.measuredDimensions[dim[axis]];
  child->layout.position[trailing[axis]] =
      node->layout.measuredDimensions[dim[axis]] - size - child->layout.position[pos[axis]];
}

// If both left and right are defined, then use left. Otherwise return
// +left or -right depending on which is defined.
static float getRelativePosition(const CSSNodeRef node, const CSSFlexDirection axis) {
  if (isLeadingPosDefined(node, axis)) {
    return getLeadingPosition(node, axis);
  }
  return -getTrailingPosition(node, axis);
}

static void setPosition(const CSSNodeRef node, const CSSDirection direction) {
  const CSSFlexDirection mainAxis = resolveAxis(node->style.flexDirection, direction);
  const CSSFlexDirection crossAxis = getCrossFlexDirection(mainAxis, direction);

  node->layout.position[leading[mainAxis]] =
      getLeadingMargin(node, mainAxis) + getRelativePosition(node, mainAxis);
  node->layout.position[trailing[mainAxis]] =
      getTrailingMargin(node, mainAxis) + getRelativePosition(node, mainAxis);
  node->layout.position[leading[crossAxis]] =
      getLeadingMargin(node, crossAxis) + getRelativePosition(node, crossAxis);
  node->layout.position[trailing[crossAxis]] =
      getTrailingMargin(node, crossAxis) + getRelativePosition(node, crossAxis);
}

static void computeChildFlexBasis(const CSSNodeRef node,
                                  const CSSNodeRef child,
                                  const float width,
                                  const CSSMeasureMode widthMode,
                                  const float height,
                                  const CSSMeasureMode heightMode,
                                  const CSSDirection direction) {
  const CSSFlexDirection mainAxis = resolveAxis(node->style.flexDirection, direction);
  const bool isMainAxisRow = isRowDirection(mainAxis);

  float childWidth;
  float childHeight;
  CSSMeasureMode childWidthMeasureMode;
  CSSMeasureMode childHeightMeasureMode;

  if (!CSSValueIsUndefined(child->style.flexBasis) &&
      !CSSValueIsUndefined(isMainAxisRow ? width : height)) {
    if (CSSValueIsUndefined(child->layout.computedFlexBasis)) {
      child->layout.computedFlexBasis =
          fmaxf(child->style.flexBasis, getPaddingAndBorderAxis(child, mainAxis));
    }
  } else if (isMainAxisRow && isStyleDimDefined(child, CSSFlexDirectionRow)) {
    // The width is definite, so use that as the flex basis.
    child->layout.computedFlexBasis = fmaxf(child->style.dimensions[CSSDimensionWidth],
                                            getPaddingAndBorderAxis(child, CSSFlexDirectionRow));
  } else if (!isMainAxisRow && isStyleDimDefined(child, CSSFlexDirectionColumn)) {
    // The height is definite, so use that as the flex basis.
    child->layout.computedFlexBasis = fmaxf(child->style.dimensions[CSSDimensionHeight],
                                            getPaddingAndBorderAxis(child, CSSFlexDirectionColumn));
  } else {
    // Compute the flex basis and hypothetical main size (i.e. the clamped
    // flex basis).
    childWidth = CSSUndefined;
    childHeight = CSSUndefined;
    childWidthMeasureMode = CSSMeasureModeUndefined;
    childHeightMeasureMode = CSSMeasureModeUndefined;

    if (isStyleDimDefined(child, CSSFlexDirectionRow)) {
      childWidth =
          child->style.dimensions[CSSDimensionWidth] + getMarginAxis(child, CSSFlexDirectionRow);
      childWidthMeasureMode = CSSMeasureModeExactly;
    }
    if (isStyleDimDefined(child, CSSFlexDirectionColumn)) {
      childHeight = child->style.dimensions[CSSDimensionHeight] +
                    getMarginAxis(child, CSSFlexDirectionColumn);
      childHeightMeasureMode = CSSMeasureModeExactly;
    }

    // The W3C spec doesn't say anything about the 'overflow' property,
    // but all major browsers appear to implement the following logic.
    if ((!isMainAxisRow && node->style.overflow == CSSOverflowScroll) ||
        node->style.overflow != CSSOverflowScroll) {
      if (CSSValueIsUndefined(childWidth) && !CSSValueIsUndefined(width)) {
        childWidth = width;
        childWidthMeasureMode = CSSMeasureModeAtMost;
      }
    }

    if ((isMainAxisRow && node->style.overflow == CSSOverflowScroll) ||
        node->style.overflow != CSSOverflowScroll) {
      if (CSSValueIsUndefined(childHeight) && !CSSValueIsUndefined(height)) {
        childHeight = height;
        childHeightMeasureMode = CSSMeasureModeAtMost;
      }
    }

    // If child has no defined size in the cross axis and is set to stretch,
    // set the cross
    // axis to be measured exactly with the available inner width
    if (!isMainAxisRow && !CSSValueIsUndefined(width) &&
        !isStyleDimDefined(child, CSSFlexDirectionRow) && widthMode == CSSMeasureModeExactly &&
        getAlignItem(node, child) == CSSAlignStretch) {
      childWidth = width;
      childWidthMeasureMode = CSSMeasureModeExactly;
    }
    if (isMainAxisRow && !CSSValueIsUndefined(height) &&
        !isStyleDimDefined(child, CSSFlexDirectionColumn) && heightMode == CSSMeasureModeExactly &&
        getAlignItem(node, child) == CSSAlignStretch) {
      childHeight = height;
      childHeightMeasureMode = CSSMeasureModeExactly;
    }

    // Measure the child
    layoutNodeInternal(child,
                       childWidth,
                       childHeight,
                       direction,
                       childWidthMeasureMode,
                       childHeightMeasureMode,
                       false,
                       "measure");

    child->layout.computedFlexBasis =
        fmaxf(isMainAxisRow ? child->layout.measuredDimensions[CSSDimensionWidth]
                            : child->layout.measuredDimensions[CSSDimensionHeight],
              getPaddingAndBorderAxis(child, mainAxis));
  }
}

static void absoluteLayoutChild(const CSSNodeRef node,
                                const CSSNodeRef child,
                                const float width,
                                const CSSMeasureMode widthMode,
                                const CSSDirection direction) {
  const CSSFlexDirection mainAxis = resolveAxis(node->style.flexDirection, direction);
  const CSSFlexDirection crossAxis = getCrossFlexDirection(mainAxis, direction);
  const bool isMainAxisRow = isRowDirection(mainAxis);

  float childWidth = CSSUndefined;
  float childHeight = CSSUndefined;
  CSSMeasureMode childWidthMeasureMode = CSSMeasureModeUndefined;
  CSSMeasureMode childHeightMeasureMode = CSSMeasureModeUndefined;

  if (isStyleDimDefined(child, CSSFlexDirectionRow)) {
    childWidth =
        child->style.dimensions[CSSDimensionWidth] + getMarginAxis(child, CSSFlexDirectionRow);
  } else {
    // If the child doesn't have a specified width, compute the width based
    // on the left/right
    // offsets if they're defined.
    if (isLeadingPosDefined(child, CSSFlexDirectionRow) &&
        isTrailingPosDefined(child, CSSFlexDirectionRow)) {
      childWidth = node->layout.measuredDimensions[CSSDimensionWidth] -
                   (getLeadingBorder(node, CSSFlexDirectionRow) +
                    getTrailingBorder(node, CSSFlexDirectionRow)) -
                   (getLeadingPosition(child, CSSFlexDirectionRow) +
                    getTrailingPosition(child, CSSFlexDirectionRow));
      childWidth = boundAxis(child, CSSFlexDirectionRow, childWidth);
    }
  }

  if (isStyleDimDefined(child, CSSFlexDirectionColumn)) {
    childHeight =
        child->style.dimensions[CSSDimensionHeight] + getMarginAxis(child, CSSFlexDirectionColumn);
  } else {
    // If the child doesn't have a specified height, compute the height
    // based on the top/bottom
    // offsets if they're defined.
    if (isLeadingPosDefined(child, CSSFlexDirectionColumn) &&
        isTrailingPosDefined(child, CSSFlexDirectionColumn)) {
      childHeight = node->layout.measuredDimensions[CSSDimensionHeight] -
                    (getLeadingBorder(node, CSSFlexDirectionColumn) +
                     getTrailingBorder(node, CSSFlexDirectionColumn)) -
                    (getLeadingPosition(child, CSSFlexDirectionColumn) +
                     getTrailingPosition(child, CSSFlexDirectionColumn));
      childHeight = boundAxis(child, CSSFlexDirectionColumn, childHeight);
    }
  }

  // If we're still missing one or the other dimension, measure the content.
  if (CSSValueIsUndefined(childWidth) || CSSValueIsUndefined(childHeight)) {
    childWidthMeasureMode =
        CSSValueIsUndefined(childWidth) ? CSSMeasureModeUndefined : CSSMeasureModeExactly;
    childHeightMeasureMode =
        CSSValueIsUndefined(childHeight) ? CSSMeasureModeUndefined : CSSMeasureModeExactly;

    // According to the spec, if the main size is not definite and the
    // child's inline axis is parallel to the main axis (i.e. it's
    // horizontal), the child should be sized using "UNDEFINED" in
    // the main size. Otherwise use "AT_MOST" in the cross axis.
    if (!isMainAxisRow && CSSValueIsUndefined(childWidth) && widthMode != CSSMeasureModeUndefined) {
      childWidth = width;
      childWidthMeasureMode = CSSMeasureModeAtMost;
    }

    layoutNodeInternal(child,
                       childWidth,
                       childHeight,
                       direction,
                       childWidthMeasureMode,
                       childHeightMeasureMode,
                       false,
                       "abs-measure");
    childWidth = child->layout.measuredDimensions[CSSDimensionWidth] +
                 getMarginAxis(child, CSSFlexDirectionRow);
    childHeight = child->layout.measuredDimensions[CSSDimensionHeight] +
                  getMarginAxis(child, CSSFlexDirectionColumn);
  }

  layoutNodeInternal(child,
                     childWidth,
                     childHeight,
                     direction,
                     CSSMeasureModeExactly,
                     CSSMeasureModeExactly,
                     true,
                     "abs-layout");

  if (isTrailingPosDefined(child, mainAxis) && !isLeadingPosDefined(child, mainAxis)) {
    child->layout.position[leading[mainAxis]] = node->layout.measuredDimensions[dim[mainAxis]] -
                                                child->layout.measuredDimensions[dim[mainAxis]] -
                                                getTrailingPosition(child, mainAxis);
  }

  if (isTrailingPosDefined(child, crossAxis) && !isLeadingPosDefined(child, crossAxis)) {
    child->layout.position[leading[crossAxis]] = node->layout.measuredDimensions[dim[crossAxis]] -
                                                 child->layout.measuredDimensions[dim[crossAxis]] -
                                                 getTrailingPosition(child, crossAxis);
  }
}

//
// This is the main routine that implements a subset of the flexbox layout
// algorithm
// described in the W3C CSS documentation: https://www.w3.org/TR/css3-flexbox/.
//
// Limitations of this algorithm, compared to the full standard:
//  * Display property is always assumed to be 'flex' except for Text nodes,
//  which
//    are assumed to be 'inline-flex'.
//  * The 'zIndex' property (or any form of z ordering) is not supported. Nodes
//  are
//    stacked in document order.
//  * The 'order' property is not supported. The order of flex items is always
//  defined
//    by document order.
//  * The 'visibility' property is always assumed to be 'visible'. Values of
//  'collapse'
//    and 'hidden' are not supported.
//  * The 'wrap' property supports only 'nowrap' (which is the default) or
//  'wrap'. The
//    rarely-used 'wrap-reverse' is not supported.
//  * Rather than allowing arbitrary combinations of flexGrow, flexShrink and
//    flexBasis, this algorithm supports only the three most common
//    combinations:
//      flex: 0 is equiavlent to flex: 0 0 auto
//      flex: n (where n is a positive value) is equivalent to flex: n 1 auto
//          If POSITIVE_FLEX_IS_AUTO is 0, then it is equivalent to flex: n 0 0
//          This is faster because the content doesn't need to be measured, but
//          it's
//          less flexible because the basis is always 0 and can't be overriden
//          with
//          the width/height attributes.
//      flex: -1 (or any negative value) is equivalent to flex: 0 1 auto
//  * Margins cannot be specified as 'auto'. They must be specified in terms of
//  pixel
//    values, and the default value is 0.
//  * The 'baseline' value is not supported for alignItems and alignSelf
//  properties.
//  * Values of width, maxWidth, minWidth, height, maxHeight and minHeight must
//  be
//    specified as pixel values, not as percentages.
//  * There is no support for calculation of dimensions based on intrinsic
//  aspect ratios
//     (e.g. images).
//  * There is no support for forced breaks.
//  * It does not support vertical inline directions (top-to-bottom or
//  bottom-to-top text).
//
// Deviations from standard:
//  * Section 4.5 of the spec indicates that all flex items have a default
//  minimum
//    main size. For text blocks, for example, this is the width of the widest
//    word.
//    Calculating the minimum width is expensive, so we forego it and assume a
//    default
//    minimum main size of 0.
//  * Min/Max sizes in the main axis are not honored when resolving flexible
//  lengths.
//  * The spec indicates that the default value for 'flexDirection' is 'row',
//  but
//    the algorithm below assumes a default of 'column'.
//
// Input parameters:
//    - node: current node to be sized and layed out
//    - availableWidth & availableHeight: available size to be used for sizing
//    the node
//      or CSSUndefined if the size is not available; interpretation depends on
//      layout
//      flags
//    - parentDirection: the inline (text) direction within the parent
//    (left-to-right or
//      right-to-left)
//    - widthMeasureMode: indicates the sizing rules for the width (see below
//    for explanation)
//    - heightMeasureMode: indicates the sizing rules for the height (see below
//    for explanation)
//    - performLayout: specifies whether the caller is interested in just the
//    dimensions
//      of the node or it requires the entire node and its subtree to be layed
//      out
//      (with final positions)
//
// Details:
//    This routine is called recursively to lay out subtrees of flexbox
//    elements. It uses the
//    information in node.style, which is treated as a read-only input. It is
//    responsible for
//    setting the layout.direction and layout.measuredDimensions fields for the
//    input node as well
//    as the layout.position and layout.lineIndex fields for its child nodes.
//    The
//    layout.measuredDimensions field includes any border or padding for the
//    node but does
//    not include margins.
//
//    The spec describes four different layout modes: "fill available", "max
//    content", "min
//    content",
//    and "fit content". Of these, we don't use "min content" because we don't
//    support default
//    minimum main sizes (see above for details). Each of our measure modes maps
//    to a layout mode
//    from the spec (https://www.w3.org/TR/css3-sizing/#terms):
//      - CSSMeasureModeUndefined: max content
//      - CSSMeasureModeExactly: fill available
//      - CSSMeasureModeAtMost: fit content
//
//    When calling layoutNodeImpl and layoutNodeInternal, if the caller passes
//    an available size of
//    undefined then it must also pass a measure mode of CSSMeasureModeUndefined
//    in that dimension.
//
static void layoutNodeImpl(const CSSNodeRef node,
                           const float availableWidth,
                           const float availableHeight,
                           const CSSDirection parentDirection,
                           const CSSMeasureMode widthMeasureMode,
                           const CSSMeasureMode heightMeasureMode,
                           const bool performLayout) {
  CSS_ASSERT(CSSValueIsUndefined(availableWidth) ? widthMeasureMode == CSSMeasureModeUndefined
                                                 : true,
             "availableWidth is indefinite so widthMeasureMode must be "
             "CSSMeasureModeUndefined");
  CSS_ASSERT(CSSValueIsUndefined(availableHeight) ? heightMeasureMode == CSSMeasureModeUndefined
                                                  : true,
             "availableHeight is indefinite so heightMeasureMode must be "
             "CSSMeasureModeUndefined");

  const float paddingAndBorderAxisRow = getPaddingAndBorderAxis(node, CSSFlexDirectionRow);
  const float paddingAndBorderAxisColumn = getPaddingAndBorderAxis(node, CSSFlexDirectionColumn);
  const float marginAxisRow = getMarginAxis(node, CSSFlexDirectionRow);
  const float marginAxisColumn = getMarginAxis(node, CSSFlexDirectionColumn);

  // Set the resolved resolution in the node's layout.
  const CSSDirection direction = resolveDirection(node, parentDirection);
  node->layout.direction = direction;

  // For content (text) nodes, determine the dimensions based on the text
  // contents.
  if (node->measure && CSSNodeChildCount(node) == 0) {
    const float innerWidth = availableWidth - marginAxisRow - paddingAndBorderAxisRow;
    const float innerHeight = availableHeight - marginAxisColumn - paddingAndBorderAxisColumn;

    if (widthMeasureMode == CSSMeasureModeExactly && heightMeasureMode == CSSMeasureModeExactly) {
      // Don't bother sizing the text if both dimensions are already defined.
      node->layout.measuredDimensions[CSSDimensionWidth] =
          boundAxis(node, CSSFlexDirectionRow, availableWidth - marginAxisRow);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node, CSSFlexDirectionColumn, availableHeight - marginAxisColumn);
    } else if (innerWidth <= 0 || innerHeight <= 0) {
      // Don't bother sizing the text if there's no horizontal or vertical
      // space.
      node->layout.measuredDimensions[CSSDimensionWidth] = boundAxis(node, CSSFlexDirectionRow, 0);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node, CSSFlexDirectionColumn, 0);
    } else {
      // Measure the text under the current constraints.
      const CSSSize measuredSize =
          node->measure(node->context, innerWidth, widthMeasureMode, innerHeight, heightMeasureMode);

      node->layout.measuredDimensions[CSSDimensionWidth] =
          boundAxis(node,
                    CSSFlexDirectionRow,
                    (widthMeasureMode == CSSMeasureModeUndefined ||
                     widthMeasureMode == CSSMeasureModeAtMost)
                        ? measuredSize.width + paddingAndBorderAxisRow
                        : availableWidth - marginAxisRow);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node,
                    CSSFlexDirectionColumn,
                    (heightMeasureMode == CSSMeasureModeUndefined ||
                     heightMeasureMode == CSSMeasureModeAtMost)
                        ? measuredSize.height + paddingAndBorderAxisColumn
                        : availableHeight - marginAxisColumn);
    }

    return;
  }

  // For nodes with no children, use the available values if they were provided,
  // or
  // the minimum size as indicated by the padding and border sizes.
  const uint32_t childCount = CSSNodeListCount(node->children);
  if (childCount == 0) {
    node->layout.measuredDimensions[CSSDimensionWidth] =
        boundAxis(node,
                  CSSFlexDirectionRow,
                  (widthMeasureMode == CSSMeasureModeUndefined ||
                   widthMeasureMode == CSSMeasureModeAtMost)
                      ? paddingAndBorderAxisRow
                      : availableWidth - marginAxisRow);
    node->layout.measuredDimensions[CSSDimensionHeight] =
        boundAxis(node,
                  CSSFlexDirectionColumn,
                  (heightMeasureMode == CSSMeasureModeUndefined ||
                   heightMeasureMode == CSSMeasureModeAtMost)
                      ? paddingAndBorderAxisColumn
                      : availableHeight - marginAxisColumn);
    return;
  }

  // If we're not being asked to perform a full layout, we can handle a number
  // of common
  // cases here without incurring the cost of the remaining function.
  if (!performLayout) {
    // If we're being asked to size the content with an at most constraint but
    // there is no available
    // width,
    // the measurement will always be zero.
    if (widthMeasureMode == CSSMeasureModeAtMost && availableWidth <= 0 &&
        heightMeasureMode == CSSMeasureModeAtMost && availableHeight <= 0) {
      node->layout.measuredDimensions[CSSDimensionWidth] = boundAxis(node, CSSFlexDirectionRow, 0);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node, CSSFlexDirectionColumn, 0);
      return;
    }

    if (widthMeasureMode == CSSMeasureModeAtMost && availableWidth <= 0) {
      node->layout.measuredDimensions[CSSDimensionWidth] = boundAxis(node, CSSFlexDirectionRow, 0);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node,
                    CSSFlexDirectionColumn,
                    CSSValueIsUndefined(availableHeight) ? 0
                                                         : (availableHeight - marginAxisColumn));
      return;
    }

    if (heightMeasureMode == CSSMeasureModeAtMost && availableHeight <= 0) {
      node->layout.measuredDimensions[CSSDimensionWidth] =
          boundAxis(node,
                    CSSFlexDirectionRow,
                    CSSValueIsUndefined(availableWidth) ? 0 : (availableWidth - marginAxisRow));
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node, CSSFlexDirectionColumn, 0);
      return;
    }

    // If we're being asked to use an exact width/height, there's no need to
    // measure the children.
    if (widthMeasureMode == CSSMeasureModeExactly && heightMeasureMode == CSSMeasureModeExactly) {
      node->layout.measuredDimensions[CSSDimensionWidth] =
          boundAxis(node, CSSFlexDirectionRow, availableWidth - marginAxisRow);
      node->layout.measuredDimensions[CSSDimensionHeight] =
          boundAxis(node, CSSFlexDirectionColumn, availableHeight - marginAxisColumn);
      return;
    }
  }

  // STEP 1: CALCULATE VALUES FOR REMAINDER OF ALGORITHM
  const CSSFlexDirection mainAxis = resolveAxis(node->style.flexDirection, direction);
  const CSSFlexDirection crossAxis = getCrossFlexDirection(mainAxis, direction);
  const bool isMainAxisRow = isRowDirection(mainAxis);
  const CSSJustify justifyContent = node->style.justifyContent;
  const bool isNodeFlexWrap = node->style.flexWrap == CSSWrapTypeWrap;

  CSSNodeRef firstAbsoluteChild = NULL;
  CSSNodeRef currentAbsoluteChild = NULL;

  const float leadingPaddingAndBorderMain = getLeadingPaddingAndBorder(node, mainAxis);
  const float trailingPaddingAndBorderMain = getTrailingPaddingAndBorder(node, mainAxis);
  const float leadingPaddingAndBorderCross = getLeadingPaddingAndBorder(node, crossAxis);
  const float paddingAndBorderAxisMain = getPaddingAndBorderAxis(node, mainAxis);
  const float paddingAndBorderAxisCross = getPaddingAndBorderAxis(node, crossAxis);

  const CSSMeasureMode measureModeMainDim = isMainAxisRow ? widthMeasureMode : heightMeasureMode;
  const CSSMeasureMode measureModeCrossDim = isMainAxisRow ? heightMeasureMode : widthMeasureMode;

  // STEP 2: DETERMINE AVAILABLE SIZE IN MAIN AND CROSS DIRECTIONS
  const float availableInnerWidth = availableWidth - marginAxisRow - paddingAndBorderAxisRow;
  const float availableInnerHeight =
      availableHeight - marginAxisColumn - paddingAndBorderAxisColumn;
  const float availableInnerMainDim = isMainAxisRow ? availableInnerWidth : availableInnerHeight;
  const float availableInnerCrossDim = isMainAxisRow ? availableInnerHeight : availableInnerWidth;

  // STEP 3: DETERMINE FLEX BASIS FOR EACH ITEM
  for (uint32_t i = 0; i < childCount; i++) {
    const CSSNodeRef child = CSSNodeListGet(node->children, i);

    if (performLayout) {
      // Set the initial position (relative to the parent).
      const CSSDirection childDirection = resolveDirection(child, direction);
      setPosition(child, childDirection);
    }

    // Absolute-positioned children don't participate in flex layout. Add them
    // to a list that we can process later.
    if (child->style.positionType == CSSPositionTypeAbsolute) {
      // Store a private linked list of absolutely positioned children
      // so that we can efficiently traverse them later.
      if (firstAbsoluteChild == NULL) {
        firstAbsoluteChild = child;
      }
      if (currentAbsoluteChild != NULL) {
        currentAbsoluteChild->nextChild = child;
      }
      currentAbsoluteChild = child;
      child->nextChild = NULL;
    } else {
      computeChildFlexBasis(node,
                            child,
                            availableInnerWidth,
                            widthMeasureMode,
                            availableInnerHeight,
                            heightMeasureMode,
                            direction);
    }
  }

  // STEP 4: COLLECT FLEX ITEMS INTO FLEX LINES

  // Indexes of children that represent the first and last items in the line.
  uint32_t startOfLineIndex = 0;
  uint32_t endOfLineIndex = 0;

  // Number of lines.
  uint32_t lineCount = 0;

  // Accumulated cross dimensions of all lines so far.
  float totalLineCrossDim = 0;

  // Max main dimension of all the lines.
  float maxLineMainDim = 0;

  for (; endOfLineIndex < childCount; lineCount++, startOfLineIndex = endOfLineIndex) {
    // Number of items on the currently line. May be different than the
    // difference
    // between start and end indicates because we skip over absolute-positioned
    // items.
    uint32_t itemsOnLine = 0;

    // sizeConsumedOnCurrentLine is accumulation of the dimensions and margin
    // of all the children on the current line. This will be used in order to
    // either set the dimensions of the node if none already exist or to compute
    // the remaining space left for the flexible children.
    float sizeConsumedOnCurrentLine = 0;

    float totalFlexGrowFactors = 0;
    float totalFlexShrinkScaledFactors = 0;

    // Maintain a linked list of the child nodes that can shrink and/or grow.
    CSSNodeRef firstRelativeChild = NULL;
    CSSNodeRef currentRelativeChild = NULL;

    // Add items to the current line until it's full or we run out of items.
    for (uint32_t i = startOfLineIndex; i < childCount; i++, endOfLineIndex++) {
      const CSSNodeRef child = CSSNodeListGet(node->children, i);
      child->lineIndex = lineCount;

      if (child->style.positionType != CSSPositionTypeAbsolute) {
        const float outerFlexBasis =
            child->layout.computedFlexBasis + getMarginAxis(child, mainAxis);

        // If this is a multi-line flow and this item pushes us over the
        // available size, we've
        // hit the end of the current line. Break out of the loop and lay out
        // the current line.
        if (sizeConsumedOnCurrentLine + outerFlexBasis > availableInnerMainDim && isNodeFlexWrap &&
            itemsOnLine > 0) {
          break;
        }

        sizeConsumedOnCurrentLine += outerFlexBasis;
        itemsOnLine++;

        if (isFlex(child)) {
          totalFlexGrowFactors += child->style.flexGrow;

          // Unlike the grow factor, the shrink factor is scaled relative to the
          // child
          // dimension.
          totalFlexShrinkScaledFactors +=
              -child->style.flexShrink * child->layout.computedFlexBasis;
        }

        // Store a private linked list of children that need to be layed out.
        if (firstRelativeChild == NULL) {
          firstRelativeChild = child;
        }
        if (currentRelativeChild != NULL) {
          currentRelativeChild->nextChild = child;
        }
        currentRelativeChild = child;
        child->nextChild = NULL;
      }
    }

    // If we don't need to measure the cross axis, we can skip the entire flex
    // step.
    const bool canSkipFlex = !performLayout && measureModeCrossDim == CSSMeasureModeExactly;

    // In order to position the elements in the main axis, we have two
    // controls. The space between the beginning and the first element
    // and the space between each two elements.
    float leadingMainDim = 0;
    float betweenMainDim = 0;

    // STEP 5: RESOLVING FLEXIBLE LENGTHS ON MAIN AXIS
    // Calculate the remaining available space that needs to be allocated.
    // If the main dimension size isn't known, it is computed based on
    // the line length, so there's no more space left to distribute.
    float remainingFreeSpace = 0;
    if (!CSSValueIsUndefined(availableInnerMainDim)) {
      remainingFreeSpace = availableInnerMainDim - sizeConsumedOnCurrentLine;
    } else if (sizeConsumedOnCurrentLine < 0) {
      // availableInnerMainDim is indefinite which means the node is being sized
      // based on its
      // content.
      // sizeConsumedOnCurrentLine is negative which means the node will
      // allocate 0 pixels for
      // its content. Consequently, remainingFreeSpace is 0 -
      // sizeConsumedOnCurrentLine.
      remainingFreeSpace = -sizeConsumedOnCurrentLine;
    }

    const float originalRemainingFreeSpace = remainingFreeSpace;
    float deltaFreeSpace = 0;

    if (!canSkipFlex) {
      float childFlexBasis;
      float flexShrinkScaledFactor;
      float flexGrowFactor;
      float baseMainSize;
      float boundMainSize;

      // Do two passes over the flex items to figure out how to distribute the
      // remaining space.
      // The first pass finds the items whose min/max constraints trigger,
      // freezes them at those
      // sizes, and excludes those sizes from the remaining space. The second
      // pass sets the size
      // of each flexible item. It distributes the remaining space amongst the
      // items whose min/max
      // constraints didn't trigger in pass 1. For the other items, it sets
      // their sizes by forcing
      // their min/max constraints to trigger again.
      //
      // This two pass approach for resolving min/max constraints deviates from
      // the spec. The
      // spec (https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths)
      // describes a process
      // that needs to be repeated a variable number of times. The algorithm
      // implemented here
      // won't handle all cases but it was simpler to implement and it mitigates
      // performance
      // concerns because we know exactly how many passes it'll do.

      // First pass: detect the flex items whose min/max constraints trigger
      float deltaFlexShrinkScaledFactors = 0;
      float deltaFlexGrowFactors = 0;
      currentRelativeChild = firstRelativeChild;
      while (currentRelativeChild != NULL) {
        childFlexBasis = currentRelativeChild->layout.computedFlexBasis;

        if (remainingFreeSpace < 0) {
          flexShrinkScaledFactor = -currentRelativeChild->style.flexShrink * childFlexBasis;

          // Is this child able to shrink?
          if (flexShrinkScaledFactor != 0) {
            baseMainSize =
                childFlexBasis +
                remainingFreeSpace / totalFlexShrinkScaledFactors * flexShrinkScaledFactor;
            boundMainSize = boundAxis(currentRelativeChild, mainAxis, baseMainSize);
            if (baseMainSize != boundMainSize) {
              // By excluding this item's size and flex factor from remaining,
              // this item's
              // min/max constraints should also trigger in the second pass
              // resulting in the
              // item's size calculation being identical in the first and second
              // passes.
              deltaFreeSpace -= boundMainSize - childFlexBasis;
              deltaFlexShrinkScaledFactors -= flexShrinkScaledFactor;
            }
          }
        } else if (remainingFreeSpace > 0) {
          flexGrowFactor = currentRelativeChild->style.flexGrow;

          // Is this child able to grow?
          if (flexGrowFactor != 0) {
            baseMainSize =
                childFlexBasis + remainingFreeSpace / totalFlexGrowFactors * flexGrowFactor;
            boundMainSize = boundAxis(currentRelativeChild, mainAxis, baseMainSize);
            if (baseMainSize != boundMainSize) {
              // By excluding this item's size and flex factor from remaining,
              // this item's
              // min/max constraints should also trigger in the second pass
              // resulting in the
              // item's size calculation being identical in the first and second
              // passes.
              deltaFreeSpace -= boundMainSize - childFlexBasis;
              deltaFlexGrowFactors -= flexGrowFactor;
            }
          }
        }

        currentRelativeChild = currentRelativeChild->nextChild;
      }

      totalFlexShrinkScaledFactors += deltaFlexShrinkScaledFactors;
      totalFlexGrowFactors += deltaFlexGrowFactors;
      remainingFreeSpace += deltaFreeSpace;

      // Second pass: resolve the sizes of the flexible items
      deltaFreeSpace = 0;
      currentRelativeChild = firstRelativeChild;
      while (currentRelativeChild != NULL) {
        childFlexBasis = currentRelativeChild->layout.computedFlexBasis;
        float updatedMainSize = childFlexBasis;

        if (remainingFreeSpace < 0) {
          flexShrinkScaledFactor = -currentRelativeChild->style.flexShrink * childFlexBasis;
          // Is this child able to shrink?
          if (flexShrinkScaledFactor != 0) {
            float childSize;

            if (totalFlexShrinkScaledFactors == 0) {
              childSize = childFlexBasis + flexShrinkScaledFactor;
            } else {
              childSize =
                  childFlexBasis +
                  (remainingFreeSpace / totalFlexShrinkScaledFactors) * flexShrinkScaledFactor;
            }

            updatedMainSize = boundAxis(currentRelativeChild, mainAxis, childSize);
          }
        } else if (remainingFreeSpace > 0) {
          flexGrowFactor = currentRelativeChild->style.flexGrow;

          // Is this child able to grow?
          if (flexGrowFactor != 0) {
            updatedMainSize =
                boundAxis(currentRelativeChild,
                          mainAxis,
                          childFlexBasis +
                              remainingFreeSpace / totalFlexGrowFactors * flexGrowFactor);
          }
        }

        deltaFreeSpace -= updatedMainSize - childFlexBasis;

        float childWidth;
        float childHeight;
        CSSMeasureMode childWidthMeasureMode;
        CSSMeasureMode childHeightMeasureMode;

        if (isMainAxisRow) {
          childWidth = updatedMainSize + getMarginAxis(currentRelativeChild, CSSFlexDirectionRow);
          childWidthMeasureMode = CSSMeasureModeExactly;

          if (!CSSValueIsUndefined(availableInnerCrossDim) &&
              !isStyleDimDefined(currentRelativeChild, CSSFlexDirectionColumn) &&
              heightMeasureMode == CSSMeasureModeExactly &&
              getAlignItem(node, currentRelativeChild) == CSSAlignStretch) {
            childHeight = availableInnerCrossDim;
            childHeightMeasureMode = CSSMeasureModeExactly;
          } else if (!isStyleDimDefined(currentRelativeChild, CSSFlexDirectionColumn)) {
            childHeight = availableInnerCrossDim;
            childHeightMeasureMode =
                CSSValueIsUndefined(childHeight) ? CSSMeasureModeUndefined : CSSMeasureModeAtMost;
          } else {
            childHeight = currentRelativeChild->style.dimensions[CSSDimensionHeight] +
                          getMarginAxis(currentRelativeChild, CSSFlexDirectionColumn);
            childHeightMeasureMode = CSSMeasureModeExactly;
          }
        } else {
          childHeight =
              updatedMainSize + getMarginAxis(currentRelativeChild, CSSFlexDirectionColumn);
          childHeightMeasureMode = CSSMeasureModeExactly;

          if (!CSSValueIsUndefined(availableInnerCrossDim) &&
              !isStyleDimDefined(currentRelativeChild, CSSFlexDirectionRow) &&
              widthMeasureMode == CSSMeasureModeExactly &&
              getAlignItem(node, currentRelativeChild) == CSSAlignStretch) {
            childWidth = availableInnerCrossDim;
            childWidthMeasureMode = CSSMeasureModeExactly;
          } else if (!isStyleDimDefined(currentRelativeChild, CSSFlexDirectionRow)) {
            childWidth = availableInnerCrossDim;
            childWidthMeasureMode =
                CSSValueIsUndefined(childWidth) ? CSSMeasureModeUndefined : CSSMeasureModeAtMost;
          } else {
            childWidth = currentRelativeChild->style.dimensions[CSSDimensionWidth] +
                         getMarginAxis(currentRelativeChild, CSSFlexDirectionRow);
            childWidthMeasureMode = CSSMeasureModeExactly;
          }
        }

        const bool requiresStretchLayout =
            !isStyleDimDefined(currentRelativeChild, crossAxis) &&
            getAlignItem(node, currentRelativeChild) == CSSAlignStretch;

        // Recursively call the layout algorithm for this child with the updated
        // main size.
        layoutNodeInternal(currentRelativeChild,
                           childWidth,
                           childHeight,
                           direction,
                           childWidthMeasureMode,
                           childHeightMeasureMode,
                           performLayout && !requiresStretchLayout,
                           "flex");

        currentRelativeChild = currentRelativeChild->nextChild;
      }
    }

    remainingFreeSpace = originalRemainingFreeSpace + deltaFreeSpace;

    // STEP 6: MAIN-AXIS JUSTIFICATION & CROSS-AXIS SIZE DETERMINATION

    // At this point, all the children have their dimensions set in the main
    // axis.
    // Their dimensions are also set in the cross axis with the exception of
    // items
    // that are aligned "stretch". We need to compute these stretch values and
    // set the final positions.

    // If we are using "at most" rules in the main axis. Calculate the remaining space when
    // constraint by the min size defined for the main axis.

    if (measureModeMainDim == CSSMeasureModeAtMost && remainingFreeSpace > 0) {
      if (!CSSValueIsUndefined(node->style.minDimensions[dim[mainAxis]]) &&
          node->style.minDimensions[dim[mainAxis]] >= 0) {
        remainingFreeSpace = fmax(0,
                                  node->style.minDimensions[dim[mainAxis]] -
                                      (availableInnerMainDim - remainingFreeSpace));
      } else {
        remainingFreeSpace = 0;
      }
    }

    switch (justifyContent) {
      case CSSJustifyCenter:
        leadingMainDim = remainingFreeSpace / 2;
        break;
      case CSSJustifyFlexEnd:
        leadingMainDim = remainingFreeSpace;
        break;
      case CSSJustifySpaceBetween:
        if (itemsOnLine > 1) {
          betweenMainDim = fmaxf(remainingFreeSpace, 0) / (itemsOnLine - 1);
        } else {
          betweenMainDim = 0;
        }
        break;
      case CSSJustifySpaceAround:
        // Space on the edges is half of the space between elements
        betweenMainDim = remainingFreeSpace / itemsOnLine;
        leadingMainDim = betweenMainDim / 2;
        break;
      case CSSJustifyFlexStart:
        break;
    }

    float mainDim = leadingPaddingAndBorderMain + leadingMainDim;
    float crossDim = 0;

    for (uint32_t i = startOfLineIndex; i < endOfLineIndex; i++) {
      const CSSNodeRef child = CSSNodeListGet(node->children, i);

      if (child->style.positionType == CSSPositionTypeAbsolute &&
          isLeadingPosDefined(child, mainAxis)) {
        if (performLayout) {
          // In case the child is position absolute and has left/top being
          // defined, we override the position to whatever the user said
          // (and margin/border).
          child->layout.position[pos[mainAxis]] = getLeadingPosition(child, mainAxis) +
                                                  getLeadingBorder(node, mainAxis) +
                                                  getLeadingMargin(child, mainAxis);
        }
      } else {
        if (performLayout) {
          // If the child is position absolute (without top/left) or relative,
          // we put it at the current accumulated offset.
          child->layout.position[pos[mainAxis]] += mainDim;
        }

        // Now that we placed the element, we need to update the variables.
        // We need to do that only for relative elements. Absolute elements
        // do not take part in that phase.
        if (child->style.positionType == CSSPositionTypeRelative) {
          if (canSkipFlex) {
            // If we skipped the flex step, then we can't rely on the
            // measuredDims because
            // they weren't computed. This means we can't call getDimWithMargin.
            mainDim +=
                betweenMainDim + getMarginAxis(child, mainAxis) + child->layout.computedFlexBasis;
            crossDim = availableInnerCrossDim;
          } else {
            // The main dimension is the sum of all the elements dimension plus
            // the spacing.
            mainDim += betweenMainDim + getDimWithMargin(child, mainAxis);

            // The cross dimension is the max of the elements dimension since
            // there
            // can only be one element in that cross dimension.
            crossDim = fmaxf(crossDim, getDimWithMargin(child, crossAxis));
          }
        }
      }
    }

    mainDim += trailingPaddingAndBorderMain;

    float containerCrossAxis = availableInnerCrossDim;
    if (measureModeCrossDim == CSSMeasureModeUndefined ||
        measureModeCrossDim == CSSMeasureModeAtMost) {
      // Compute the cross axis from the max cross dimension of the children.
      containerCrossAxis = boundAxis(node, crossAxis, crossDim + paddingAndBorderAxisCross) -
                           paddingAndBorderAxisCross;

      if (measureModeCrossDim == CSSMeasureModeAtMost) {
        containerCrossAxis = fminf(containerCrossAxis, availableInnerCrossDim);
      }
    }

    // If there's no flex wrap, the cross dimension is defined by the container.
    if (!isNodeFlexWrap && measureModeCrossDim == CSSMeasureModeExactly) {
      crossDim = availableInnerCrossDim;
    }

    // Clamp to the min/max size specified on the container.
    crossDim = boundAxis(node, crossAxis, crossDim + paddingAndBorderAxisCross) -
               paddingAndBorderAxisCross;

    // STEP 7: CROSS-AXIS ALIGNMENT
    // We can skip child alignment if we're just measuring the container.
    if (performLayout) {
      for (uint32_t i = startOfLineIndex; i < endOfLineIndex; i++) {
        const CSSNodeRef child = CSSNodeListGet(node->children, i);

        if (child->style.positionType == CSSPositionTypeAbsolute) {
          // If the child is absolutely positioned and has a
          // top/left/bottom/right
          // set, override all the previously computed positions to set it
          // correctly.
          if (isLeadingPosDefined(child, crossAxis)) {
            child->layout.position[pos[crossAxis]] = getLeadingPosition(child, crossAxis) +
                                                     getLeadingBorder(node, crossAxis) +
                                                     getLeadingMargin(child, crossAxis);
          } else {
            child->layout.position[pos[crossAxis]] =
                leadingPaddingAndBorderCross + getLeadingMargin(child, crossAxis);
          }
        } else {
          float leadingCrossDim = leadingPaddingAndBorderCross;

          // For a relative children, we're either using alignItems (parent) or
          // alignSelf (child) in order to determine the position in the cross
          // axis
          const CSSAlign alignItem = getAlignItem(node, child);

          // If the child uses align stretch, we need to lay it out one more
          // time, this time
          // forcing the cross-axis size to be the computed cross size for the
          // current line.
          if (alignItem == CSSAlignStretch) {
            const bool isCrossSizeDefinite =
                (isMainAxisRow && isStyleDimDefined(child, CSSFlexDirectionColumn)) ||
                (!isMainAxisRow && isStyleDimDefined(child, CSSFlexDirectionRow));

            float childWidth;
            float childHeight;
            CSSMeasureMode childWidthMeasureMode;
            CSSMeasureMode childHeightMeasureMode;

            if (isMainAxisRow) {
              childHeight = crossDim;
              childWidth = child->layout.measuredDimensions[CSSDimensionWidth] +
                           getMarginAxis(child, CSSFlexDirectionRow);
            } else {
              childWidth = crossDim;
              childHeight = child->layout.measuredDimensions[CSSDimensionHeight] +
                            getMarginAxis(child, CSSFlexDirectionColumn);
            }

            // If the child defines a definite size for its cross axis, there's
            // no need to stretch.
            if (!isCrossSizeDefinite) {
              childWidthMeasureMode =
                  CSSValueIsUndefined(childWidth) ? CSSMeasureModeUndefined : CSSMeasureModeExactly;
              childHeightMeasureMode = CSSValueIsUndefined(childHeight) ? CSSMeasureModeUndefined
                                                                        : CSSMeasureModeExactly;
              layoutNodeInternal(child,
                                 childWidth,
                                 childHeight,
                                 direction,
                                 childWidthMeasureMode,
                                 childHeightMeasureMode,
                                 true,
                                 "stretch");
            }
          } else if (alignItem != CSSAlignFlexStart) {
            const float remainingCrossDim = containerCrossAxis - getDimWithMargin(child, crossAxis);

            if (alignItem == CSSAlignCenter) {
              leadingCrossDim += remainingCrossDim / 2;
            } else { // CSSAlignFlexEnd
              leadingCrossDim += remainingCrossDim;
            }
          }

          // And we apply the position
          child->layout.position[pos[crossAxis]] += totalLineCrossDim + leadingCrossDim;
        }
      }
    }

    totalLineCrossDim += crossDim;
    maxLineMainDim = fmaxf(maxLineMainDim, mainDim);
  }

  // STEP 8: MULTI-LINE CONTENT ALIGNMENT
  if (lineCount > 1 && performLayout && !CSSValueIsUndefined(availableInnerCrossDim)) {
    const float remainingAlignContentDim = availableInnerCrossDim - totalLineCrossDim;

    float crossDimLead = 0;
    float currentLead = leadingPaddingAndBorderCross;

    switch (node->style.alignContent) {
      case CSSAlignFlexEnd:
        currentLead += remainingAlignContentDim;
        break;
      case CSSAlignCenter:
        currentLead += remainingAlignContentDim / 2;
        break;
      case CSSAlignStretch:
        if (availableInnerCrossDim > totalLineCrossDim) {
          crossDimLead = (remainingAlignContentDim / lineCount);
        }
        break;
      case CSSAlignAuto:
      case CSSAlignFlexStart:
        break;
    }

    uint32_t endIndex = 0;
    for (uint32_t i = 0; i < lineCount; i++) {
      uint32_t startIndex = endIndex;
      uint32_t ii;

      // compute the line's height and find the endIndex
      float lineHeight = 0;
      for (ii = startIndex; ii < childCount; ii++) {
        const CSSNodeRef child = CSSNodeListGet(node->children, ii);

        if (child->style.positionType == CSSPositionTypeRelative) {
          if (child->lineIndex != i) {
            break;
          }

          if (isLayoutDimDefined(child, crossAxis)) {
            lineHeight = fmaxf(lineHeight,
                               child->layout.measuredDimensions[dim[crossAxis]] +
                                   getMarginAxis(child, crossAxis));
          }
        }
      }
      endIndex = ii;
      lineHeight += crossDimLead;

      if (performLayout) {
        for (ii = startIndex; ii < endIndex; ii++) {
          const CSSNodeRef child = CSSNodeListGet(node->children, ii);

          if (child->style.positionType == CSSPositionTypeRelative) {
            switch (getAlignItem(node, child)) {
              case CSSAlignFlexStart: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + getLeadingMargin(child, crossAxis);
                break;
              }
              case CSSAlignFlexEnd: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + lineHeight - getTrailingMargin(child, crossAxis) -
                    child->layout.measuredDimensions[dim[crossAxis]];
                break;
              }
              case CSSAlignCenter: {
                float childHeight = child->layout.measuredDimensions[dim[crossAxis]];
                child->layout.position[pos[crossAxis]] =
                    currentLead + (lineHeight - childHeight) / 2;
                break;
              }
              case CSSAlignStretch: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + getLeadingMargin(child, crossAxis);
                // TODO(prenaux): Correctly set the height of items with indefinite
                //                (auto) crossAxis dimension.
                break;
              }
              case CSSAlignAuto:
                break;
            }
          }
        }
      }

      currentLead += lineHeight;
    }
  }

  // STEP 9: COMPUTING FINAL DIMENSIONS
  node->layout.measuredDimensions[CSSDimensionWidth] =
      boundAxis(node, CSSFlexDirectionRow, availableWidth - marginAxisRow);
  node->layout.measuredDimensions[CSSDimensionHeight] =
      boundAxis(node, CSSFlexDirectionColumn, availableHeight - marginAxisColumn);

  // If the user didn't specify a width or height for the node, set the
  // dimensions based on the children.
  if (measureModeMainDim == CSSMeasureModeUndefined) {
    // Clamp the size to the min/max size, if specified, and make sure it
    // doesn't go below the padding and border amount.
    node->layout.measuredDimensions[dim[mainAxis]] = boundAxis(node, mainAxis, maxLineMainDim);
  } else if (measureModeMainDim == CSSMeasureModeAtMost) {
    node->layout.measuredDimensions[dim[mainAxis]] =
        fmaxf(fminf(availableInnerMainDim + paddingAndBorderAxisMain,
                    boundAxisWithinMinAndMax(node, mainAxis, maxLineMainDim)),
              paddingAndBorderAxisMain);
  }

  if (measureModeCrossDim == CSSMeasureModeUndefined) {
    // Clamp the size to the min/max size, if specified, and make sure it
    // doesn't go below the padding and border amount.
    node->layout.measuredDimensions[dim[crossAxis]] =
        boundAxis(node, crossAxis, totalLineCrossDim + paddingAndBorderAxisCross);
  } else if (measureModeCrossDim == CSSMeasureModeAtMost) {
    node->layout.measuredDimensions[dim[crossAxis]] =
        fmaxf(fminf(availableInnerCrossDim + paddingAndBorderAxisCross,
                    boundAxisWithinMinAndMax(node,
                                             crossAxis,
                                             totalLineCrossDim + paddingAndBorderAxisCross)),
              paddingAndBorderAxisCross);
  }

  if (performLayout) {
    // STEP 10: SIZING AND POSITIONING ABSOLUTE CHILDREN
    for (currentAbsoluteChild = firstAbsoluteChild; currentAbsoluteChild != NULL;
         currentAbsoluteChild = currentAbsoluteChild->nextChild) {
      absoluteLayoutChild(
          node, currentAbsoluteChild, availableInnerWidth, widthMeasureMode, direction);
    }

    // STEP 11: SETTING TRAILING POSITIONS FOR CHILDREN
    const bool needsMainTrailingPos =
        mainAxis == CSSFlexDirectionRowReverse || mainAxis == CSSFlexDirectionColumnReverse;
    const bool needsCrossTrailingPos =
        CSSFlexDirectionRowReverse || crossAxis == CSSFlexDirectionColumnReverse;

    // Set trailing position if necessary.
    if (needsMainTrailingPos || needsCrossTrailingPos) {
      for (uint32_t i = 0; i < childCount; i++) {
        const CSSNodeRef child = CSSNodeListGet(node->children, i);

        if (needsMainTrailingPos) {
          setTrailingPosition(node, child, mainAxis);
        }

        if (needsCrossTrailingPos) {
          setTrailingPosition(node, child, crossAxis);
        }
      }
    }
  }
}

uint32_t gDepth = 0;
bool gPrintTree = false;
bool gPrintChanges = false;
bool gPrintSkips = false;

static const char *spacer = "                                                            ";

static const char *getSpacer(const unsigned long level) {
  const unsigned long spacerLen = strlen(spacer);
  if (level > spacerLen) {
    return &spacer[0];
  } else {
    return &spacer[spacerLen - level];
  }
}

static const char *getModeName(const CSSMeasureMode mode, const bool performLayout) {
  const char *kMeasureModeNames[CSSMeasureModeCount] = {"UNDEFINED", "EXACTLY", "AT_MOST"};
  const char *kLayoutModeNames[CSSMeasureModeCount] = {"LAY_UNDEFINED",
                                                       "LAY_EXACTLY",
                                                       "LAY_AT_"
                                                       "MOST"};

  if (mode >= CSSMeasureModeCount) {
    return "";
  }

  return performLayout ? kLayoutModeNames[mode] : kMeasureModeNames[mode];
}

static bool canUseCachedMeasurement(const bool isTextNode,
                                    const float availableWidth,
                                    const float availableHeight,
                                    const float marginRow,
                                    const float marginColumn,
                                    const CSSMeasureMode widthMeasureMode,
                                    const CSSMeasureMode heightMeasureMode,
                                    CSSCachedMeasurement cachedLayout) {
  const bool isHeightSame = (cachedLayout.heightMeasureMode == CSSMeasureModeUndefined &&
                             heightMeasureMode == CSSMeasureModeUndefined) ||
                            (cachedLayout.heightMeasureMode == heightMeasureMode &&
                             eq(cachedLayout.availableHeight, availableHeight));

  const bool isWidthSame = (cachedLayout.widthMeasureMode == CSSMeasureModeUndefined &&
                            widthMeasureMode == CSSMeasureModeUndefined) ||
                           (cachedLayout.widthMeasureMode == widthMeasureMode &&
                            eq(cachedLayout.availableWidth, availableWidth));

  if (isHeightSame && isWidthSame) {
    return true;
  }

  const bool isHeightValid = (cachedLayout.heightMeasureMode == CSSMeasureModeUndefined &&
                              heightMeasureMode == CSSMeasureModeAtMost &&
                              cachedLayout.computedHeight <= (availableHeight - marginColumn)) ||
                             (heightMeasureMode == CSSMeasureModeExactly &&
                              eq(cachedLayout.computedHeight, availableHeight - marginColumn));

  if (isWidthSame && isHeightValid) {
    return true;
  }

  const bool isWidthValid = (cachedLayout.widthMeasureMode == CSSMeasureModeUndefined &&
                             widthMeasureMode == CSSMeasureModeAtMost &&
                             cachedLayout.computedWidth <= (availableWidth - marginRow)) ||
                            (widthMeasureMode == CSSMeasureModeExactly &&
                             eq(cachedLayout.computedWidth, availableWidth - marginRow));

  if (isHeightSame && isWidthValid) {
    return true;
  }

  if (isHeightValid && isWidthValid) {
    return true;
  }

  // We know this to be text so we can apply some more specialized heuristics.
  if (isTextNode) {
    if (isWidthSame) {
      if (heightMeasureMode == CSSMeasureModeUndefined) {
        // Width is the same and height is not restricted. Re-use cahced value.
        return true;
      }

      if (heightMeasureMode == CSSMeasureModeAtMost &&
          cachedLayout.computedHeight < (availableHeight - marginColumn)) {
        // Width is the same and height restriction is greater than the cached
        // height. Re-use cached
        // value.
        return true;
      }

      // Width is the same but height restriction imposes smaller height than
      // previously measured.
      // Update the cached value to respect the new height restriction.
      cachedLayout.computedHeight = availableHeight - marginColumn;
      return true;
    }

    if (cachedLayout.widthMeasureMode == CSSMeasureModeUndefined) {
      if (widthMeasureMode == CSSMeasureModeUndefined ||
          (widthMeasureMode == CSSMeasureModeAtMost &&
           cachedLayout.computedWidth <= (availableWidth - marginRow))) {
        // Previsouly this text was measured with no width restriction, if width
        // is now restricted
        // but to a larger value than the previsouly measured width we can
        // re-use the measurement
        // as we know it will fit.
        return true;
      }
    }
  }

  return false;
}

//
// This is a wrapper around the layoutNodeImpl function. It determines
// whether the layout request is redundant and can be skipped.
//
// Parameters:
//  Input parameters are the same as layoutNodeImpl (see above)
//  Return parameter is true if layout was performed, false if skipped
//
bool layoutNodeInternal(const CSSNodeRef node,
                        const float availableWidth,
                        const float availableHeight,
                        const CSSDirection parentDirection,
                        const CSSMeasureMode widthMeasureMode,
                        const CSSMeasureMode heightMeasureMode,
                        const bool performLayout,
                        const char *reason) {
  CSSLayout *layout = &node->layout;

  gDepth++;

  const bool needToVisitNode =
      (node->isDirty && layout->generationCount != gCurrentGenerationCount) ||
      layout->lastParentDirection != parentDirection;

  if (needToVisitNode) {
    // Invalidate the cached results.
    layout->nextCachedMeasurementsIndex = 0;
    layout->cachedLayout.widthMeasureMode = (CSSMeasureMode) -1;
    layout->cachedLayout.heightMeasureMode = (CSSMeasureMode) -1;
  }

  CSSCachedMeasurement *cachedResults = NULL;

  // Determine whether the results are already cached. We maintain a separate
  // cache for layouts and measurements. A layout operation modifies the
  // positions
  // and dimensions for nodes in the subtree. The algorithm assumes that each
  // node
  // gets layed out a maximum of one time per tree layout, but multiple
  // measurements
  // may be required to resolve all of the flex dimensions.
  // We handle nodes with measure functions specially here because they are the
  // most
  // expensive to measure, so it's worth avoiding redundant measurements if at
  // all possible.
  if (node->measure && CSSNodeChildCount(node) == 0) {
    const float marginAxisRow = getMarginAxis(node, CSSFlexDirectionRow);
    const float marginAxisColumn = getMarginAxis(node, CSSFlexDirectionColumn);

    // First, try to use the layout cache.
    if (canUseCachedMeasurement(node->isTextNode,
                                availableWidth,
                                availableHeight,
                                marginAxisRow,
                                marginAxisColumn,
                                widthMeasureMode,
                                heightMeasureMode,
                                layout->cachedLayout)) {
      cachedResults = &layout->cachedLayout;
    } else {
      // Try to use the measurement cache.
      for (uint32_t i = 0; i < layout->nextCachedMeasurementsIndex; i++) {
        if (canUseCachedMeasurement(node->isTextNode,
                                    availableWidth,
                                    availableHeight,
                                    marginAxisRow,
                                    marginAxisColumn,
                                    widthMeasureMode,
                                    heightMeasureMode,
                                    layout->cachedMeasurements[i])) {
          cachedResults = &layout->cachedMeasurements[i];
          break;
        }
      }
    }
  } else if (performLayout) {
    if (eq(layout->cachedLayout.availableWidth, availableWidth) &&
        eq(layout->cachedLayout.availableHeight, availableHeight) &&
        layout->cachedLayout.widthMeasureMode == widthMeasureMode &&
        layout->cachedLayout.heightMeasureMode == heightMeasureMode) {
      cachedResults = &layout->cachedLayout;
    }
  } else {
    for (uint32_t i = 0; i < layout->nextCachedMeasurementsIndex; i++) {
      if (eq(layout->cachedMeasurements[i].availableWidth, availableWidth) &&
          eq(layout->cachedMeasurements[i].availableHeight, availableHeight) &&
          layout->cachedMeasurements[i].widthMeasureMode == widthMeasureMode &&
          layout->cachedMeasurements[i].heightMeasureMode == heightMeasureMode) {
        cachedResults = &layout->cachedMeasurements[i];
        break;
      }
    }
  }

  if (!needToVisitNode && cachedResults != NULL) {
    layout->measuredDimensions[CSSDimensionWidth] = cachedResults->computedWidth;
    layout->measuredDimensions[CSSDimensionHeight] = cachedResults->computedHeight;

    if (gPrintChanges && gPrintSkips) {
      printf("%s%d.{[skipped] ", getSpacer(gDepth), gDepth);
      if (node->print) {
        node->print(node->context);
      }
      printf("wm: %s, hm: %s, aw: %f ah: %f => d: (%f, %f) %s\n",
             getModeName(widthMeasureMode, performLayout),
             getModeName(heightMeasureMode, performLayout),
             availableWidth,
             availableHeight,
             cachedResults->computedWidth,
             cachedResults->computedHeight,
             reason);
    }
  } else {
    if (gPrintChanges) {
      printf("%s%d.{%s", getSpacer(gDepth), gDepth, needToVisitNode ? "*" : "");
      if (node->print) {
        node->print(node->context);
      }
      printf("wm: %s, hm: %s, aw: %f ah: %f %s\n",
             getModeName(widthMeasureMode, performLayout),
             getModeName(heightMeasureMode, performLayout),
             availableWidth,
             availableHeight,
             reason);
    }

    layoutNodeImpl(node,
                   availableWidth,
                   availableHeight,
                   parentDirection,
                   widthMeasureMode,
                   heightMeasureMode,
                   performLayout);

    if (gPrintChanges) {
      printf("%s%d.}%s", getSpacer(gDepth), gDepth, needToVisitNode ? "*" : "");
      if (node->print) {
        node->print(node->context);
      }
      printf("wm: %s, hm: %s, d: (%f, %f) %s\n",
             getModeName(widthMeasureMode, performLayout),
             getModeName(heightMeasureMode, performLayout),
             layout->measuredDimensions[CSSDimensionWidth],
             layout->measuredDimensions[CSSDimensionHeight],
             reason);
    }

    layout->lastParentDirection = parentDirection;

    if (cachedResults == NULL) {
      if (layout->nextCachedMeasurementsIndex == CSS_MAX_CACHED_RESULT_COUNT) {
        if (gPrintChanges) {
          printf("Out of cache entries!\n");
        }
        layout->nextCachedMeasurementsIndex = 0;
      }

      CSSCachedMeasurement *newCacheEntry;
      if (performLayout) {
        // Use the single layout cache entry.
        newCacheEntry = &layout->cachedLayout;
      } else {
        // Allocate a new measurement cache entry.
        newCacheEntry = &layout->cachedMeasurements[layout->nextCachedMeasurementsIndex];
        layout->nextCachedMeasurementsIndex++;
      }

      newCacheEntry->availableWidth = availableWidth;
      newCacheEntry->availableHeight = availableHeight;
      newCacheEntry->widthMeasureMode = widthMeasureMode;
      newCacheEntry->heightMeasureMode = heightMeasureMode;
      newCacheEntry->computedWidth = layout->measuredDimensions[CSSDimensionWidth];
      newCacheEntry->computedHeight = layout->measuredDimensions[CSSDimensionHeight];
    }
  }

  if (performLayout) {
    node->layout.dimensions[CSSDimensionWidth] = node->layout.measuredDimensions[CSSDimensionWidth];
    node->layout.dimensions[CSSDimensionHeight] =
        node->layout.measuredDimensions[CSSDimensionHeight];
    node->hasNewLayout = true;
    node->isDirty = false;
  }

  gDepth--;
  layout->generationCount = gCurrentGenerationCount;
  return (needToVisitNode || cachedResults == NULL);
}

void CSSNodeCalculateLayout(const CSSNodeRef node,
                            const float availableWidth,
                            const float availableHeight,
                            const CSSDirection parentDirection) {
  // Increment the generation count. This will force the recursive routine to
  // visit
  // all dirty nodes at least once. Subsequent visits will be skipped if the
  // input
  // parameters don't change.
  gCurrentGenerationCount++;

  float width = availableWidth;
  float height = availableHeight;
  CSSMeasureMode widthMeasureMode = CSSMeasureModeUndefined;
  CSSMeasureMode heightMeasureMode = CSSMeasureModeUndefined;

  if (!CSSValueIsUndefined(width)) {
    widthMeasureMode = CSSMeasureModeExactly;
  } else if (isStyleDimDefined(node, CSSFlexDirectionRow)) {
    width =
        node->style.dimensions[dim[CSSFlexDirectionRow]] + getMarginAxis(node, CSSFlexDirectionRow);
    widthMeasureMode = CSSMeasureModeExactly;
  } else if (node->style.maxDimensions[CSSDimensionWidth] >= 0.0) {
    width = node->style.maxDimensions[CSSDimensionWidth];
    widthMeasureMode = CSSMeasureModeAtMost;
  }

  if (!CSSValueIsUndefined(height)) {
    heightMeasureMode = CSSMeasureModeExactly;
  } else if (isStyleDimDefined(node, CSSFlexDirectionColumn)) {
    height = node->style.dimensions[dim[CSSFlexDirectionColumn]] +
             getMarginAxis(node, CSSFlexDirectionColumn);
    heightMeasureMode = CSSMeasureModeExactly;
  } else if (node->style.maxDimensions[CSSDimensionHeight] >= 0.0) {
    height = node->style.maxDimensions[CSSDimensionHeight];
    heightMeasureMode = CSSMeasureModeAtMost;
  }

  if (layoutNodeInternal(node,
                         width,
                         height,
                         parentDirection,
                         widthMeasureMode,
                         heightMeasureMode,
                         true,
                         "initia"
                         "l")) {
    setPosition(node, node->layout.direction);

    if (gPrintTree) {
      CSSNodePrint(node, CSSPrintOptionsLayout | CSSPrintOptionsChildren | CSSPrintOptionsStyle);
    }
  }
}

void CSSLayoutSetLogger(CSSLogger logger) {
  gLogger = logger;
}

#ifdef CSS_ASSERT_FAIL_ENABLED
static CSSAssertFailFunc gAssertFailFunc;

void CSSAssertSetFailFunc(CSSAssertFailFunc func) {
  gAssertFailFunc = func;
}

void CSSAssertFail(const char *message) {
  if (gAssertFailFunc) {
    (*gAssertFailFunc)(message);
  }
}
#endif
