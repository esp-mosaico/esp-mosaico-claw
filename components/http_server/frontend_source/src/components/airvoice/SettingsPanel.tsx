import { createEffect, createSignal, For, Show, type Component, type JSX } from 'solid-js';
import { createStore, reconcile, unwrap } from 'solid-js/store';
import { airVoiceApi, type AirVoiceConfig } from '../../api/airvoice';
import { t, tf, type TKey } from '../../i18n';
import { useDirtyTracker } from '../../state/dirty';
import { pushToast } from '../../state/toast';
import { Button } from '../ui/Button';
import { SelectInput, TextInput } from '../ui/FormField';
import { SavePanel } from '../ui/SavePanel';
import { Switch } from '../ui/Switch';

const PGA_STEPS = Array.from({ length: 11 }, (_, i) => i * 3);
const ADC_STEPS = Array.from({ length: 8 }, (_, i) => i * 6);
const HPF_OPTIONS = [0, 60, 80, 100, 120, 150, 200, 250, 300];

type Props = {
  config: AirVoiceConfig | null;
  presets: string[];
  onSaved: (config: AirVoiceConfig, presets: string[]) => void;
};

type RangeProps = {
  label: JSX.Element;
  value: number;
  min: number;
  max: number;
  step: number;
  disabled?: boolean;
  onInput: (value: number) => void;
};

const Range: Component<RangeProps> = (props) => (
  <label class="flex flex-col gap-1 text-[0.8rem] text-[var(--color-text-secondary)]">
    <span>{props.label}</span>
    <input
      type="range"
      class="!p-0 !border-0 !bg-transparent accent-[var(--color-accent)]"
      min={props.min}
      max={props.max}
      step={props.step}
      value={props.value}
      disabled={props.disabled}
      onInput={(event) => props.onInput(Number(event.currentTarget.value))}
    />
  </label>
);

const Group: Component<{ title: JSX.Element; children: JSX.Element }> = (props) => (
  <div class="flex flex-col gap-3 py-3">
    <span class="text-[0.78rem] uppercase tracking-wider font-semibold text-[var(--color-text-muted)]">
      {props.title}
    </span>
    {props.children}
  </div>
);

export const SettingsPanel: Component<Props> = (props) => {
  const [form, setForm] = createStore<AirVoiceConfig>({} as AirVoiceConfig);
  const [saving, setSaving] = createSignal(false);
  const [baseline, setBaseline] = createSignal('');

  createEffect(() => {
    const cfg = props.config;
    if (!cfg) return;
    setForm(reconcile({ ...cfg }));
    setBaseline(JSON.stringify(cfg));
  });

  const dirty = () => baseline() !== '' && JSON.stringify(unwrap(form)) !== baseline();
  useDirtyTracker('airvoice', dirty);

  const set = <K extends keyof AirVoiceConfig>(key: K, value: AirVoiceConfig[K]) => {
    setForm(key, value as never);
    if (key !== 'preset') setForm('preset', 'custom');
  };

  const save = async () => {
    setSaving(true);
    try {
      const res = await airVoiceApi.saveConfig({ ...unwrap(form) });
      props.onSaved(res.config, res.presets);
      pushToast(t('avApplied') as string, 'success');
    } catch (err) {
      pushToast((err as Error).message, 'error', 5000);
    } finally {
      setSaving(false);
    }
  };

  const applyPreset = async (name: string) => {
    setSaving(true);
    try {
      const res = await airVoiceApi.applyPreset(name);
      props.onSaved(res.config, res.presets);
      pushToast(t('avApplied') as string, 'success');
    } catch (err) {
      pushToast((err as Error).message, 'error', 5000);
    } finally {
      setSaving(false);
    }
  };

  const presetLabel = (name: string) => {
    const key = `avPreset_${name}` as TKey;
    const text = t(key) as string | undefined;
    return text && text !== key ? text : name;
  };

  return (
    <Show when={props.config}>
      <div class="pb-2 divide-y divide-[var(--color-border-subtle)]">
        <Group title={t('avPresets')}>
          <div class="flex flex-wrap gap-2">
            <For each={props.presets}>
              {(name) => (
                <Button
                  size="sm"
                  variant="secondary"
                  active={form.preset === name}
                  disabled={saving()}
                  onClick={() => void applyPreset(name)}
                >
                  {presetLabel(name)}
                </Button>
              )}
            </For>
            <Show when={!props.presets.includes(form.preset)}>
              <span class="self-center text-[0.78rem] text-[var(--color-orange)]">
                {t('avCustom')}
              </span>
            </Show>
          </div>
        </Group>

        <Group title={t('avGainStaging')}>
          <div class="grid gap-3 sm:grid-cols-2">
            <SelectInput
              label={t('avPga')}
              hint={t('avPgaHint')}
              value={String(form.pga_db)}
              onChange={(event) => set('pga_db', Number(event.currentTarget.value))}
            >
              <For each={PGA_STEPS}>{(db) => <option value={db}>{db} dB</option>}</For>
            </SelectInput>
            <SelectInput
              label={t('avAdcGain')}
              hint={t('avAdcHint')}
              value={String(form.adc_gain_db)}
              onChange={(event) => set('adc_gain_db', Number(event.currentTarget.value))}
            >
              <For each={ADC_STEPS}>{(db) => <option value={db}>{db} dB</option>}</For>
            </SelectInput>
          </div>
          <span class="text-[0.78rem] text-[var(--color-text-secondary)]">
            {tf('avTotalGain', { db: form.pga_db + form.adc_gain_db })}
          </span>
        </Group>

        <Group title={t('avEnhance')}>
          <div class="grid gap-4 sm:grid-cols-2">
            <SelectInput
              label={t('avHpf')}
              value={String(form.hpf_hz)}
              onChange={(event) => set('hpf_hz', Number(event.currentTarget.value))}
            >
              <For each={HPF_OPTIONS}>
                {(hz) => <option value={hz}>{hz === 0 ? t('avOff') : `${hz} Hz`}</option>}
              </For>
            </SelectInput>
            <div class="flex flex-col gap-2">
              <Switch
                label={t('avAirflowSwitch') as string}
                checked={form.airflow_enable}
                onChange={(v) => set('airflow_enable', v)}
              />
              <Range
                label={tf('avAirflowDepth', { db: form.airflow_depth_db })}
                value={form.airflow_depth_db}
                min={0}
                max={36}
                step={1}
                disabled={!form.airflow_enable}
                onInput={(v) => set('airflow_depth_db', v)}
              />
            </div>
            <div class="flex flex-col gap-2">
              <Switch
                label={t('avNs') as string}
                checked={form.ns_enable}
                onChange={(v) => set('ns_enable', v)}
              />
              <Range
                label={tf('avNsDepth', { db: form.ns_depth_db })}
                value={form.ns_depth_db}
                min={0}
                max={30}
                step={1}
                disabled={!form.ns_enable}
                onInput={(v) => set('ns_depth_db', v)}
              />
            </div>
            <div class="flex flex-col gap-2">
              <Switch
                label={t('avEq') as string}
                checked={form.eq_enable}
                onChange={(v) => set('eq_enable', v)}
              />
              <Range
                label={tf('avEqGain', { db: form.eq_presence_db })}
                value={form.eq_presence_db}
                min={-6}
                max={12}
                step={1}
                disabled={!form.eq_enable}
                onInput={(v) => set('eq_presence_db', v)}
              />
            </div>
            <div class="flex flex-col gap-2">
              <Switch
                label={t('avAgc') as string}
                checked={form.agc_enable}
                onChange={(v) => set('agc_enable', v)}
              />
              <Show
                when={form.agc_enable}
                fallback={
                  <Range
                    label={tf('avManualGain', { db: form.manual_gain_db })}
                    value={form.manual_gain_db}
                    min={-20}
                    max={40}
                    step={1}
                    onInput={(v) => set('manual_gain_db', v)}
                  />
                }
              >
                <Range
                  label={tf('avAgcTarget', { db: form.agc_target_dbfs })}
                  value={form.agc_target_dbfs}
                  min={-36}
                  max={-6}
                  step={1}
                  onInput={(v) => set('agc_target_dbfs', v)}
                />
                <Range
                  label={tf('avAgcMax', { db: form.agc_max_gain_db })}
                  value={form.agc_max_gain_db}
                  min={0}
                  max={42}
                  step={1}
                  onInput={(v) => set('agc_max_gain_db', v)}
                />
              </Show>
            </div>
            <div class="flex flex-col gap-2">
              <Switch
                label={t('avLimiter') as string}
                checked={form.limiter_enable}
                onChange={(v) => set('limiter_enable', v)}
              />
              <Range
                label={tf('avGate', { db: form.gate_db })}
                value={form.gate_db}
                min={0}
                max={20}
                step={1}
                onInput={(v) => set('gate_db', v)}
              />
              <Range
                label={tf('avVadThreshold', { db: form.vad_threshold_db })}
                value={form.vad_threshold_db}
                min={1}
                max={12}
                step={0.5}
                onInput={(v) => set('vad_threshold_db', v)}
              />
            </div>
          </div>
        </Group>

        <Group title={t('avRecording')}>
          <div class="grid gap-3 sm:grid-cols-2 items-end">
            <Switch
              label={t('avSaveRaw') as string}
              checked={form.save_raw}
              onChange={(v) => set('save_raw', v)}
            />
            <TextInput
              type="number"
              min={10}
              max={3600}
              label={t('avMaxRecord')}
              value={String(form.max_record_s)}
              onChange={(event) => set('max_record_s', Number(event.currentTarget.value) || 600)}
            />
          </div>
        </Group>
      </div>
      <div class="-mx-5 -mb-5">
        <SavePanel
          dirty={dirty()}
          saving={saving()}
          onSave={() => void save()}
          onDiscard={() => props.config && setForm(reconcile({ ...props.config }))}
          saveLabel={t('avApply') as string}
        />
      </div>
    </Show>
  );
};
