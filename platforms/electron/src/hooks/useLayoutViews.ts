import { useState, useEffect, useMemo, useCallback } from 'react';
import type { DrawData, Batch, TextEntity, ViewDefinition, Bounds } from '../app/types';

export interface LayoutTab {
  id: string;
  name: string;
  type: 'model' | 'layout';
  view?: ViewDefinition;
}

export interface LayoutViewsResult {
  currentViewId: string;
  activeView: ViewDefinition | undefined;
  filteredBatches: Batch[];
  filteredTexts: TextEntity[];
  layoutTabs: LayoutTab[];
  fitBounds: Bounds | undefined;
  isPaperMode: boolean;
  switchView: (viewId: string) => void;
}

const MODEL_TAB_ID = '__model__';

function filterBySpace<T extends { space?: string; layoutId?: number }>(
  items: T[], isModel: boolean, layoutId?: number,
): T[] {
  if (isModel) {
    // Model tab: show model-space and untagged items
    return items.filter(b => b.space === undefined || b.space === 'model' || b.space === 'unknown');
  }
  // Layout tab: prefer paper-space items matching layoutId.
  // If no paper batches exist (layout renders model-space geometry through viewport),
  // fall back to all items so the layout canvas isn't blank.
  const paper = items.filter(b => {
    if (b.space !== 'paper') return false;
    if (layoutId !== undefined && b.layoutId !== undefined) return b.layoutId === layoutId;
    return true;
  });
  if (paper.length > 0) return paper;
  return items;
}

function deriveTabs(views?: ViewDefinition[]): LayoutTab[] {
  const tabs: LayoutTab[] = [{ id: MODEL_TAB_ID, name: 'Model', type: 'model' }];
  if (!views) return tabs;
  const seen = new Set<number>();
  for (const v of views) {
    if (v.type !== 'layout' || v.layoutIndex === undefined) continue;
    if (seen.has(v.layoutIndex)) continue;
    seen.add(v.layoutIndex);
    tabs.push({
      id: v.id,
      name: v.name || `Layout${v.layoutIndex + 1}`,
      type: 'layout',
      view: v,
    });
  }
  return tabs;
}

function resolveInitialView(views?: ViewDefinition[], activeViewId?: string): string {
  if (!views?.length) return MODEL_TAB_ID;
  if (!activeViewId) return MODEL_TAB_ID;
  const active = views.find(v => v.id === activeViewId);
  if (!active) return MODEL_TAB_ID;
  if (active.type === 'layout') return activeViewId;
  if (active.type === 'layoutViewport' && active.layoutIndex !== undefined) {
    const parent = views.find(v => v.type === 'layout' && v.layoutIndex === active.layoutIndex);
    if (parent) return parent.id;
  }
  return MODEL_TAB_ID;
}

export function useLayoutViews(drawData: DrawData): LayoutViewsResult {
  const [currentViewId, setCurrentViewId] = useState(MODEL_TAB_ID);

  // Sync with drawData.activeViewId on data change
  useEffect(() => {
    setCurrentViewId(resolveInitialView(drawData.views, drawData.activeViewId));
  }, [drawData.views, drawData.activeViewId]);

  const layoutTabs = useMemo(() => deriveTabs(drawData.views), [drawData.views]);

  const activeTab = useMemo(
    () => layoutTabs.find(t => t.id === currentViewId) ?? layoutTabs[0],
    [layoutTabs, currentViewId],
  );

  const isModel = activeTab.type === 'model';
  const layoutId = activeTab.view?.layoutIndex;

  const filteredBatches = useMemo(
    () => filterBySpace(drawData.batches, isModel, layoutId),
    [drawData.batches, isModel, layoutId],
  );

  const filteredTexts = useMemo(
    () => filterBySpace(drawData.texts, isModel, layoutId),
    [drawData.texts, isModel, layoutId],
  );

  const activeView = activeTab.view;

  const fitBounds = useMemo(() => {
    if (isModel) return undefined;
    const v = activeTab.view;
    if (!v) return undefined;
    const valid = (b?: Bounds) => b && b.minX < b.maxX && b.minY < b.maxY;
    return (valid(v.plotWindow) ? v.plotWindow : undefined) ??
           (valid(v.paperBounds) ? v.paperBounds : undefined) ??
           (valid(v.presentationBounds) ? v.presentationBounds : undefined) ??
           v.bounds;
  }, [isModel, activeTab.view]);

  const switchView = useCallback((viewId: string) => {
    setCurrentViewId(viewId);
  }, []);

  return {
    currentViewId,
    activeView,
    filteredBatches,
    filteredTexts,
    layoutTabs,
    fitBounds,
    isPaperMode: !isModel,
    switchView,
  };
}
