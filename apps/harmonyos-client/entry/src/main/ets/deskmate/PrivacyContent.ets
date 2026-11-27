export interface PrivacySection {
  title: string;
  body: string;
}

export const APP_DISPLAY_NAME: string = 'DeskMate';
export const FALLBACK_BUNDLE_NAME: string = 'com.supremacy.deskmate';
export const FALLBACK_VERSION_NAME: string = '1.7.0';
export const FALLBACK_VERSION_CODE: number = 1007000;
export const DEVELOPER_NAME: string = 'SupremacySakura';
export const CONTACT_EMAIL: string = '3118654731@qq.com';
export const PRIVACY_UPDATED_DATE: string = '2026-11-27';

export const PRIVACY_ONE_LINE_COMMITMENT: string =
  '画面与触控仅在本地 USB 连接中实时处理，不联网、不上传、不存储。';

export const PRIVACY_CONSENT_HIGHLIGHTS: Array<PrivacySection> = [
  {
    title: '不申请网络权限',
    body: '本应用为纯本地单机客户端，不接入广告、统计分析或第三方联网 SDK。'
  },
  {
    title: '本地 USB 实时处理',
    body: '仅在您同意后枚举并请求 USB 配件授权，桌面画面与触控坐标均在本地内存中实时流转，不落盘、不上传。'
  },
  {
    title: '随时可拒绝或撤销',
    body: '未同意前不会开始 USB 配件枚举；您可随时断开数据线或在系统设置中撤回授权、卸载应用。'
  }
];

export const PRIVACY_POLICY_SECTIONS: Array<PrivacySection> = [
  {
    title: '1. 我们收集的信息',
    body: 'DeskMate 不收集、不上传、不存储您的个人信息。应用不申请网络权限，不接入广告、统计分析或第三方 SDK。'
  },
  {
    title: '2. 本地处理的数据',
    body: '为实现有线扩展屏功能，应用会通过 USB 配件连接接收电脑端传输的画面，并将您在手机或平板上的触控坐标回传给电脑端。上述数据仅在本地内存中实时处理，不写入本地存储，不通过互联网传输，不与我们或任何第三方共享。'
  },
  {
    title: '3. 设备与权限',
    body: '应用会在您同意本隐私提示后访问 USB 配件列表并请求 USB 配件授权，用于建立本地有线连接。授权由系统弹窗完成，您可以随时拒绝或断开 USB 数据线。应用不申请位置、通讯录、相册、麦克风、摄像头等权限。'
  },
  {
    title: '4. 数据共享与跨境传输',
    body: '我们不共享、不转让、不公开披露您的数据，也不进行数据跨境传输。'
  },
  {
    title: '5. 未成年人保护',
    body: '应用不面向未成年人单独收集信息。'
  },
  {
    title: '6. 您的权利',
    body: `您可以随时断开 USB 或卸载应用以停止数据处理；如有疑问，请联系 ${CONTACT_EMAIL}。`
  },
  {
    title: '7. 政策更新',
    body: '如本政策发生变更，我们会在应用内或应用市场页面同步更新。'
  }
];
