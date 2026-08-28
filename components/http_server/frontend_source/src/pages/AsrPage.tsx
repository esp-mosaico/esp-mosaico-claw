import { createEffect, createSignal, Show, type Component } from 'solid-js';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { t } from '../i18n';
import { pushToast } from '../state/toast';

type PresetKey = 'qwen' | 'trial';

type AsrForm = {
  asr_provider: string;
  asr_api_key: string;
  asr_workspace_id: string;
  asr_language_hint: string;
  asr_model: string;
  asr_endpoint: string;
};

const PRESETS: Record<PresetKey, Omit<AsrForm, 'asr_api_key' | 'asr_workspace_id'>> = {
  qwen: {
    asr_provider: 'qwen',
    asr_language_hint: 'zh',
    asr_model: 'fun-asr-realtime',
    asr_endpoint: 'wss://dashscope.aliyuncs.com/api-ws/v1/inference',
  },
  trial: {
    asr_provider: 'trial',
    asr_language_hint: 'zh',
    asr_model: 'device-asr',
    asr_endpoint: 'wss://as1.esp-claw.com/v1/asr/realtime',
  },
};

export const AsrPage: Component = () => {
  const tab = createConfigTab<AsrForm>({
    tab: 'asr',
    groups: ['asr'],
    toForm: (config: Partial<AppConfig>) => ({
      asr_provider: config.asr_provider ?? 'qwen',
      asr_api_key: config.asr_api_key ?? '',
      asr_workspace_id: config.asr_workspace_id ?? '',
      asr_language_hint: config.asr_language_hint ?? 'zh',
      asr_model: config.asr_model ?? 'fun-asr-realtime',
      asr_endpoint: config.asr_endpoint ?? '',
    }),
    fromForm: (form) => ({
      asr_provider: form.asr_provider.trim(),
      asr_api_key: form.asr_api_key.trim(),
      asr_workspace_id: form.asr_workspace_id.trim(),
      asr_language_hint: form.asr_language_hint.trim(),
      asr_model: form.asr_model.trim(),
      asr_endpoint: form.asr_endpoint.trim(),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);
  const [selectedPreset, setSelectedPreset] = createSignal<PresetKey | null>(null);

  createEffect(() => {
    void tab.form.asr_provider;
    void tab.form.asr_api_key;
    void tab.form.asr_workspace_id;
    void tab.form.asr_language_hint;
    void tab.form.asr_model;
    void tab.form.asr_endpoint;
    setValidationError(null);
  });

  const applyPreset = (key: PresetKey) => {
    const preset = PRESETS[key];
    if (key === 'trial') {
      tab.setForm('asr_api_key', '');
    }
    tab.setForm('asr_provider', preset.asr_provider);
    tab.setForm('asr_language_hint', preset.asr_language_hint);
    tab.setForm('asr_model', preset.asr_model);
    tab.setForm('asr_endpoint', preset.asr_endpoint);
    setSelectedPreset(key);
  };

  const handleSave = async () => {
    const required: Array<[string, string]> = [
      [tab.form.asr_provider, t('asrProvider') as string],
      [tab.form.asr_model, t('asrModel') as string],
      [tab.form.asr_endpoint, t('asrEndpoint') as string],
    ];
    if (tab.form.asr_provider !== 'trial') {
      required.push([tab.form.asr_api_key, t('asrApiKey') as string]);
    }
    const missing = required.filter(([value]) => !value.trim()).map(([, label]) => label);
    if (missing.length > 0) {
      const message = (t('asrValidationRequiredFields') as string).replace(
        '{fields}',
        missing.join(' / '),
      );
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }
    await tab.save();
  };

  return (
    <TabShell>
      <PageHeader title={t('navAsr') as string} description={t('asrSection') as string} />
      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('asrPresets') as string}>
          <div class="flex flex-wrap gap-2 pt-2">
            <Button
              size="sm"
              variant="secondary"
              active={selectedPreset() === 'qwen'}
              onClick={() => applyPreset('qwen')}
            >
              {t('asrProviderQwen')}
            </Button>
            <Button
              size="sm"
              variant="secondary"
              active={selectedPreset() === 'trial'}
              onClick={() => applyPreset('trial')}
            >
              {t('asrProviderTrial')}
            </Button>
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('asrSection') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('asrProvider')}
              value={tab.form.asr_provider}
              onInput={(event) => tab.setForm('asr_provider', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('asrApiKey')}
              value={tab.form.asr_api_key}
              onInput={(event) => tab.setForm('asr_api_key', event.currentTarget.value)}
            />
            <TextInput
              label={t('asrWorkspace')}
              value={tab.form.asr_workspace_id}
              onInput={(event) => tab.setForm('asr_workspace_id', event.currentTarget.value)}
            />
            <TextInput
              label={t('asrLanguage')}
              value={tab.form.asr_language_hint}
              onInput={(event) => tab.setForm('asr_language_hint', event.currentTarget.value)}
            />
            <TextInput
              label={t('asrModel')}
              value={tab.form.asr_model}
              onInput={(event) => tab.setForm('asr_model', event.currentTarget.value)}
            />
            <TextInput
              type="url"
              label={t('asrEndpoint')}
              value={tab.form.asr_endpoint}
              onInput={(event) => tab.setForm('asr_endpoint', event.currentTarget.value)}
            />
          </div>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
